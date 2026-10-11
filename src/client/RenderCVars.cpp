// The render options the client registers at startup: the full-screen effects (anti-aliasing,
// colour correction and colour blindness, FUN_1405dcf20), the frame rate target (FUN_14023caf0),
// FSR sharpening (FUN_1412a2bf0) and the ambient occlusion tuning (FUN_142557ae0).

#include "client/RenderCVars.hpp"
#include "client/RenderSettings.hpp"
#include "client/Resample.hpp"
#include "console/Command.hpp"
#include "console/Var.hpp"
#include "gx/Gx.hpp"
#include "gx/GxDevice.hpp"
#include "os/Os.hpp"
#include "storm/String.hpp"
#include "util/Fnv.hpp"
#include "util/Unimplemented.hpp"
#include <cstdlib>

uint32_t GraphicsCheckSsaoType(int32_t type);

namespace {

// The full-screen effects' state (DAT_142e96f50.., DAT_142cce30c..).
uint8_t s_colorCorrection;              // DAT_142e96f50: brightness, contrast or gamma moved
uint16_t s_daltonize = 1;               // DAT_142e96f70
uint8_t s_colorblindSimulate;           // DAT_142e96f72
float s_colorblindWeakness;             // DAT_142e96fb0
float s_colorblindMatrix[3][4];         // DAT_142e96f80: the simulation's rows
float s_colorblindTable[9];             // DAT_142cce318: the row the simulation was set from
float s_brightness = 50.0f;             // DAT_142cce30c
float s_contrast = 50.0f;               // DAT_142cce310
float s_gamma = 1.0f;                   // DAT_142cce314
uint8_t s_ffxInitialized;               // DAT_142e96fd0
uint8_t s_ssaoInitialized;              // DAT_142d5eb50

CVar* s_ffxAntiAliasingModeCVar;        // DAT_142e96ff0
CVar* s_daltonizeCVar;                  // DAT_142e96fe8
CVar* s_colorblindWeaknessFactorCVar;   // DAT_142e96fe0
CVar* s_colorblindSimulatorCVar;        // DAT_142e96fd8
CVar* s_contrastCVar;                   // DAT_142e96ff8
CVar* s_brightnessCVar;                 // DAT_142e97000
CVar* s_gammaCVar;                      // DAT_142e97008
CVar* s_cMAA2QualityCVar;               // DAT_142e97010
CVar* s_cMAA2ExtraSharpnessCVar;        // DAT_142e97018
CVar* s_cMAA2HalfFloatCVar;             // DAT_142e97020
CVar* s_targetFPSCVar;                  // DAT_142de1978
CVar* s_useTargetFPSCVar;               // DAT_142de1980
CVar* s_dynamicRenderScaleCVar;         // DAT_142de1988
CVar* s_dynamicRenderScaleMinCVar;      // DAT_142de1990

// The colour blindness simulations' matrices (DAT_142cce3c0) and names (PTR_s_Protanope_1426c6c90).
const float s_colorblindMatrices[11][9] = {
    { 0.112f, 0.888f, 0.0f, 0.112f, 0.888f, 0.0f, 0.004f, -0.004f, 1.0f },
    { 0.293f, 0.707f, 0.0f, 0.293f, 0.707f, 0.0f, -0.022f, 0.022f, 1.0f },
    { 0.493f, 0.507f, 0.0f, 0.493f, 0.507f, 0.0f, -3.011f, 3.011f, 1.0f },
    { 0.567f, 0.433f, 0.0f, 0.558f, 0.442f, 0.0f, 0.0f, 0.242f, 0.758f },
    { 0.817f, 0.183f, 0.0f, 0.333f, 0.667f, 0.0f, 0.0f, 0.125f, 0.875f },
    { 0.625f, 0.375f, 0.0f, 0.7f, 0.3f, 0.0f, 0.0f, 0.3f, 0.7f },
    { 0.8f, 0.2f, 0.0f, 0.258f, 0.742f, 0.0f, 0.0f, 0.142f, 0.858f },
    { 0.95f, 0.05f, 0.0f, 0.0f, 0.433f, 0.567f, 0.0f, 0.475f, 0.525f },
    { 0.967f, 0.033f, 0.0f, 0.0f, 0.733f, 0.267f, 0.0f, 0.183f, 0.817f },
    { 0.299f, 0.587f, 0.114f, 0.299f, 0.587f, 0.114f, 0.299f, 0.587f, 0.114f },
    { 0.618f, 0.32f, 0.062f, 0.163f, 0.775f, 0.062f, 0.163f, 0.32f, 0.516f },
};
const char* const s_colorblindNames[11] = {
    "Protanope", "Deuteranope", "Tritanope",
    "Legacy Protanopia   (1% of males)",
    "Protanomaly         (1% of males, 0.01% of females)",
    "Legacy Deuteranopia (1% of males)",
    "Deuteranomaly       (6% of males, 0.4% of females)",
    "Legacy Tritanopia   (rare)",
    "Tritanomaly         (rare)",
    "Achromatopsia       (very rare)",
    "Achromatomaly       (very rare)",
};

long ParseLong(const char* text) {
    int32_t radix = 10;
    if (text != nullptr && text[0] == '0' && ((text[1] + 0xa8u) & 0xdf) == 0) {
        radix = 16;
    }
    return strtol(text, nullptr, radix);
}

void SetFlag(uint32_t bit, bool on) {
    g_pendingRenderSettings.m_flags = (on ? bit : 0) | (g_pendingRenderSettings.m_flags & ~bit);
}

// ---- the full-screen effects ----------------------------------------------------------------

// The correction pass runs while anything moved from 50 / 50 / 1.
void UpdateColorCorrection() {
    s_colorCorrection = !(s_brightness == 50.0f && s_contrast == 50.0f && s_gamma == 1.0f);
}

// ref: FUN_1405dc1d0
void FfxSetBrightness(float brightness) {
    if (brightness < 0.0f) {
        s_brightness = 0.0f;
        s_colorCorrection = 1;
        return;
    }
    s_brightness = 100.0f <= brightness ? 100.0f : brightness;
    UpdateColorCorrection();
}

// ref: FUN_1405dc240
void FfxSetContrast(float contrast) {
    s_contrast = 0.0f;
    if (0.0f <= contrast) {
        s_contrast = 100.0f <= contrast ? 100.0f : contrast;
    }
    UpdateColorCorrection();
}

// 0.3 .. 2.8.
// ref: FUN_1405dc2a0
void FfxSetGamma(float gamma) {
    s_gamma = 0.3f;
    if (0.3f <= gamma) {
        s_gamma = 2.8f <= gamma ? 2.8f : gamma;
    }
    UpdateColorCorrection();
}

// ref: FUN_1405dc7b0
void FfxSetDaltonize(uint32_t mode) {
    s_daltonize = mode < 2 ? static_cast<uint16_t>(mode) : 1;
}

// ref: FUN_1405dc7e0
void FfxSetColorblindWeakness(float weakness) {
    s_colorblindWeakness = weakness;
}

// ref: FUN_1405dc7d0
void FfxSetColorblindSimulate(bool simulate) {
    s_colorblindSimulate = simulate;
}

// A 3x3 colour matrix (kind 9) as the shader's three padded rows.
// ref: FUN_1405dc6e0
void FfxSetColorMatrix(int32_t kind, const float* matrix) {
    if (kind != 9) {
        return;
    }
    for (int32_t row = 0; row < 3; row++) {
        for (int32_t column = 0; column < 3; column++) {
            s_colorblindMatrix[row][column] = matrix[row * 3 + column];
        }
        s_colorblindMatrix[row][3] = 0.0f;
    }
    if (matrix != s_colorblindTable) {
        for (int32_t i = 0; i < 9; i++) {
            s_colorblindTable[i] = matrix[i];
        }
    }
}

// FXAA / CMAA2 need a device that takes them.
// ref: FUN_1405e6000
bool FfxCmaa2Supported() {
    if (GxCurrentApi() == 0) {
        return false;
    }
    auto version = reinterpret_cast<const int32_t*>(OsGetVersion());
    if (version[0] != 10 || 9u < static_cast<uint32_t>(version[1])) {
        auto device = static_cast<CGxDevice*>(g_gxDevice);
        return device->Field<uint32_t>(0x80) >= 0xb000 || device->Field<uint8_t>(0x84) >= 6;
    }
    return false;
}

// ref: FUN_1405dd8d0
bool FfxAntiAliasingModeCallback(CVar*, const char*, const char* value, void*) {
    uint32_t mode = static_cast<uint32_t>(ParseLong(value));
    uint8_t kind = static_cast<uint8_t>(mode);
    if (kind != 0 && kind != 1 && kind != 2) {
        bool supported;
        if (kind == 3) {
            // FUN_1405e5ae0: FUN_14228c060(0xb000, 4)
            auto device = static_cast<CGxDevice*>(g_gxDevice);
            supported = device->Field<uint32_t>(0x80) >= 0xb000 || device->Field<uint8_t>(0x84) >= 4;
        } else if (kind == 4) {
            supported = FfxCmaa2Supported();
        } else {
            return false;
        }
        if (!supported) {
            return false;
        }
    }
    g_pendingRenderSettings.m_antiAliasing = kind;
    return true;
}

// ref: FUN_1405dd880
bool DaltonizeCallback(CVar*, const char*, const char* value, void*) {
    FfxSetDaltonize(static_cast<uint32_t>(ParseLong(value)));
    return true;
}

// ref: FUN_1405dd840
bool ColorblindWeaknessFactorCallback(CVar*, const char*, const char* value, void*) {
    FfxSetColorblindWeakness(SStrToFloat(value));
    return true;
}

// 1 .. 11 simulates one kind of colour blindness (or corrects for it, with daltonize).
// ref: FUN_1405dd730
bool ColorblindSimulatorCallback(CVar*, const char*, const char* value, void*) {
    long mode = ParseLong(value);
    if (static_cast<unsigned long>(mode - 1) < 0xb) {
        FfxSetColorblindSimulate(true);
        FfxSetColorMatrix(9, s_colorblindMatrices[mode - 1]);
        uint32_t daltonize = static_cast<uint32_t>(s_daltonizeCVar->m_intValue);
        FfxSetDaltonize(daltonize);
        FfxSetColorblindWeakness(s_colorblindWeaknessFactorCVar->m_floatValue);
        if (1 < daltonize) {
            ConsoleWriteFormat(0, "Daltonization enabled. Mode set to %d - %s", mode, s_colorblindNames[mode - 1]);
            return true;
        }
        ConsoleWriteFormat(0, "Color blindness simulation enabled. Mode set to %d - %s", mode, s_colorblindNames[mode - 1]);
        return true;
    }
    FfxSetColorblindSimulate(false);
    ConsoleWriteText("FFX: Color Blind Test Mode Disabled");
    return true;
}

// ref: FUN_1405dd860
bool ContrastCallback(CVar*, const char*, const char* value, void*) {
    FfxSetContrast(SStrToFloat(value));
    return true;
}

// ref: FUN_1405dd710
bool BrightnessCallback(CVar*, const char*, const char* value, void*) {
    FfxSetBrightness(SStrToFloat(value));
    return true;
}

// ref: FUN_1405dd950
bool GammaCallback(CVar*, const char*, const char* value, void*) {
    FfxSetGamma(SStrToFloat(value));
    return true;
}

// ref: FUN_1405dc7f0
bool CMAA2QualityCallback(CVar*, const char*, const char* value, void*) {
    uint32_t quality = static_cast<uint32_t>(atoi(value));
    if (quality < 4) {
        g_pendingRenderSettings.m_flags ^= (quality << 0xb ^ g_pendingRenderSettings.m_flags) & 0x1800;
        return true;
    }
    return false;
}

// ref: FUN_1405dc8a0
bool CMAA2ExtraSharpnessCallback(CVar*, const char*, const char* value, void*) {
    SetFlag(0x2000, atoi(value) != 0);
    return true;
}

// ref: FUN_1405dc8d0
bool CMAA2HalfFloatCallback(CVar*, const char*, const char* value, void*) {
    SetFlag(0x4000, atoi(value) != 0);
    return true;
}

// The effects' device resources and render settings hooks.
// ref: FUN_1405dcb00
void FfxCreateDeviceObjects() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_1405e55b0
void FfxInitialize(bool) {
    // the effect materials and the glow, AA and colour passes (0x1405e55b0) are not ported
    FROST_UNIMPLEMENTED();
}

// ---- the frame rate target --------------------------------------------------------------

// The frame statistics (DAT_142de2920) sized for a target rate (FUN_14023d080).
// ref: FUN_14023d080
void FrameStatsReset(int32_t target) {
    (void)target;
    // the frame time history the statistics keep is not ported
    FROST_UNIMPLEMENTED();
}

// 0 .. 300.
// ref: FUN_14023b9a0
bool TargetFPSCallback(CVar*, const char*, const char* value, void*) {
    uint32_t target = static_cast<uint32_t>(atoi(value));
    if (300 < target) {
        return false;
    }
    FrameStatsReset(static_cast<int32_t>(target));
    return true;
}

// Turning it off puts the render scale back to the RenderScale CVar's.
// ref: FUN_14023b9e0
bool DynamicRenderScaleCallback(CVar*, const char* oldValue, const char* value, void*) {
    bool was = oldValue != nullptr && atoi(oldValue) != 0;
    int32_t now = atoi(value);
    SetFlag(0x200, now != 0);
    if (was != (now != 0) && now == 0) {
        CVar* renderScale = CVarLookup(t2util::ConstFnvHashStringI("RenderScale"));
        g_pendingRenderSettings.m_renderScale = renderScale->m_floatValue;
    }
    return true;
}

// 1/120 .. the device's maximum.
// ref: FUN_14023ba70
bool DynamicRenderScaleMinCallback(CVar*, const char*, const char* value, void*) {
    double scale = strtod(value, nullptr);
    if (static_cast<float>(scale) < 0.008333334f) {
        return false;
    }
    return static_cast<float>(scale) <= RenderSettingsMaxScale();
}

// ref: LAB_14023bac0
int32_t CommandGxFrameStats(const char*, const char*) {
    FROST_UNIMPLEMENTED();
    return 1;
}

// ref: LAB_14023bad0
int32_t CommandLogFps(const char*, const char*) {
    FROST_UNIMPLEMENTED();
    return 1;
}

// ref: FUN_14023cc90
void FrameRateCreateDeviceObjects() {
    FROST_UNIMPLEMENTED();
}

// ---- FSR sharpening ---------------------------------------------------------------------

// Up to 2; -1 turns it off.
// ref: FUN_1412a1d80
bool ResampleSharpnessCallback(CVar*, const char*, const char* value, void*) {
    double sharpness = strtod(value, nullptr);
    if (2.0 < sharpness) {
        return false;
    }
    g_pendingRenderSettings.m_sharpness = static_cast<float>(sharpness);
    return true;
}

// ref: FUN_1412a1db0
bool ResampleAlwaysSharpenCallback(CVar*, const char*, const char* value, void*) {
    SetFlag(0x400, atoi(value) != 0);
    return true;
}

// ---- ambient occlusion ----------------------------------------------------------------------

// Normals stay on where the device has no compute.
// ref: FUN_1425575d0
bool AssaoNormalsCallback(CVar* cvar, const char*, const char* value, void*) {
    if (reinterpret_cast<int32_t*>(GxDeviceCaps())[2] == 0 && value != nullptr && value[0] == '0') {
        CVarSet(cvar, "1", true, false, false, true);
        return false;
    }
    return true;
}

// Once ambient occlusion is up, a newly resolved type is passed on to SSAOType.
// ref: FUN_142558060
bool ResolvedSSAOTypeCallback(CVar*, const char* oldValue, const char* value, void*) {
    if (s_ssaoInitialized != 0) {
        int32_t was = atoi(oldValue);
        int32_t now = atoi(value);
        if (was != now && GraphicsCheckSsaoType(now) == 0) {
            CVar* type = CVarLookup(t2util::ConstFnvHashStringI("SSAOType"));
            if (type != nullptr) {
                char text[0x20];
                SStrPrintf(text, sizeof(text), "%lli", static_cast<long long>(now));
                return CVarSet(type, text, true, false, false, true);
            }
        }
    }
    return true;
}

CVar* s_assaoNormalsCVar;
CVar* s_assaoRadiusCVar;
CVar* s_assaoShadowMultCVar;
CVar* s_assaoShadowPowerCVar;
CVar* s_assaoShadowClampCVar;
CVar* s_assaoHorizonAngleThreshCVar;
CVar* s_assaoFadeOutFromCVar;
CVar* s_assaoFadeOutToCVar;
CVar* s_assaoAdaptiveQualityLimitCVar;
CVar* s_assaoBlurPassCountCVar;
CVar* s_assaoSharpnessCVar;
CVar* s_assaoTemporalSSAngleOffsetCVar;
CVar* s_assaoTemporalSSRadiusOffsetCVar;
CVar* s_assaoDetailShadowStrengthCVar;
CVar* s_cacaoBilateralSimilarityDistanceSigmaCVar;
CVar* s_ssaoMagicNormalsCVar;
CVar* s_ssaoMagicThresholdLowCVar;
CVar* s_ssaoMagicThresholdHighCVar;
CVar* s_resolvedSSAOTypeCVar;

}

// ref: FUN_1405dcf20
void FullScreenEffectsRegisterCVars() {
    s_ffxInitialized = 0;
    // FUN_14228dfb0 / FUN_14228e040 (the device create and destroy callbacks: FUN_1405dcb00,
    // FUN_1405dcde0, LAB_1405dca80, FUN_1405dcab0, FUN_1405dd6d0, FUN_1405dd6f0) and
    // FUN_14129d3e0 / FUN_14129d420 (the render settings validator FUN_1405dd320 and handler
    // FUN_1405dd440) belong to the effect passes, which are not ported.
    char contrast[0x40];
    char brightness[0x40];
    char gamma[0x40];
    // FUN_1423626a0: a float in fixed notation
    SStrPrintf(contrast, sizeof(contrast), "%.10f", 50.0);
    SStrPrintf(brightness, sizeof(brightness), "%.10f", 50.0);
    SStrPrintf(gamma, sizeof(gamma), "%.10f", 1.0);
    s_ffxAntiAliasingModeCVar = CVarRegister("ffxAntiAliasingMode", "Anti Aliasing Mode", 1, "0", FfxAntiAliasingModeCallback, 1, false, nullptr, false);
    s_daltonizeCVar = CVarRegister("daltonize", "Attempt to correct for color blindness (set colorblindSimulator to type of colorblindness)", 9, "1", DaltonizeCallback, 1, false, nullptr, false);
    s_colorblindWeaknessFactorCVar = CVarRegister("colorblindWeaknessFactor", "Amount of sensitivity. e.g. Protanope (red-weakness) 0.0 = not colorblind, 1.0 = full weakness(Protanopia), 0.5 = mid weakness(Protanomaly)", 9, "0.5", ColorblindWeaknessFactorCallback, 1, false, nullptr, false);
    s_colorblindSimulatorCVar = CVarRegister("colorblindSimulator", "Type of color blindness", 9, "0", ColorblindSimulatorCallback, 1, false, nullptr, false);
    s_contrastCVar = CVarRegister("Contrast", "Contrast adjustment. Range: [0 - 100]", 1, contrast, ContrastCallback, 1, false, nullptr, false);
    s_brightnessCVar = CVarRegister("Brightness", "Brightness adjustment. Range: [0 - 100]", 1, brightness, BrightnessCallback, 1, false, nullptr, false);
    s_gammaCVar = CVarRegister("Gamma", "Gamma correction. Range: [0.3 - 2.8]", 1, gamma, GammaCallback, 1, false, nullptr, false);
    s_cMAA2QualityCVar = CVarRegister("CMAA2Quality", "CMAA2 Quality Level. 0 - LOW, 1 - MEDIUM, 2 - HIGH, 3 - ULTRA", 1, "3", CMAA2QualityCallback, 1, false, nullptr, false);
    s_cMAA2ExtraSharpnessCVar = CVarRegister("CMAA2ExtraSharpness", "Set to 1 to preserve even more text and shape clarity at the expense of less AA", 1, "0", CMAA2ExtraSharpnessCallback, 1, false, nullptr, false);
    s_cMAA2HalfFloatCVar = CVarRegister("CMAA2HalfFloat", "0: 32-bit Float. 1: 16-bit Float.", 1, "0", CMAA2HalfFloatCallback, 1, false, nullptr, false);
    FfxInitialize(true);
}

// ref: FUN_14023caf0
void FrameRateRegisterCVars() {
    s_targetFPSCVar = CVarRegister("targetFPS", "Set target FPS. Dynamic actions will be taken if you fall below the FPS target", 1, "60", TargetFPSCallback, 1, false, nullptr, false);
    s_useTargetFPSCVar = CVarRegister("useTargetFPS", "Enables or disables background FPS limit", 1, "1", nullptr, 1, false, nullptr, false);
    s_dynamicRenderScaleCVar = CVarRegister("DynamicRenderScale", "Lowers render scale if GPU bound to hit Target FPS. Note this feature is in BETA. Known issues: May cause hitching. May behave poorly with vsync on.", 1, "0", DynamicRenderScaleCallback, 1, false, nullptr, false);
    s_dynamicRenderScaleMinCVar = CVarRegister("DynamicRenderScaleMin", "Lowest render scale DynamicRenderScale can use", 1, "0.33333333", DynamicRenderScaleMinCallback, 1, false, nullptr, false);
    CVarSetFloat(s_dynamicRenderScaleMinCVar, 0.33333334f, true, true, true, true);
    ConsoleCommandRegister("GxFrameStats", CommandGxFrameStats, 1, "Get Engine Frame Stats", 0);
    ConsoleCommandRegister("LogFps", CommandLogFps, 1,
        "Log FPS data to file. Optionally pass in polling freq in seconds, default 1 second. Call again to end logging.", 0);
    // FUN_14228dfb0(FUN_14023cc90): the frame rate overlay's device objects (not ported)
}

// ref: FUN_1412a2bf0
void ResampleRegisterCVars() {
    GxAddDeviceCreatedCallback(ResampleDeviceCreated, nullptr);
    GxAddDeviceDestroyedCallback(ResampleDeviceDestroyed, nullptr);
    CVarRegister("ResampleSharpness", "FSR sharpness strength [0.0-2.0]. 0 is full strength. -1 to disable.", 1, "0.2", ResampleSharpnessCallback, 1, false, nullptr, false);
    CVarRegister("ResampleAlwaysSharpen", "Run sharpness pass, even if not using AMD FSR Upscale [0,1]", 1, "0", ResampleAlwaysSharpenCallback, 1, false, nullptr, false);
}

// ref: FUN_142557ae0
void AmbientOcclusionRegisterCVars() {
    s_assaoNormalsCVar = CVarRegister("assaoNormals", "Use Normals for ASSAO", 1, "1", AssaoNormalsCallback, 1, false, nullptr, false);
    s_assaoRadiusCVar = CVarRegister("assaoRadius", "ASSAO Radius [0.0, ~ ] World (view) space size of the occlusion sphere", 1, "1.85", nullptr, 1, false, nullptr, false);
    s_assaoShadowMultCVar = CVarRegister("assaoShadowMult", "ASSAO Shadow Multiplier [0.0, 5.0] Effect strength linear multiplier", 1, "1.1", nullptr, 1, false, nullptr, false);
    s_assaoShadowPowerCVar = CVarRegister("assaoShadowPower", "ASSAO Shadow Power [0.5, 5.0] Effect strength pow modifier", 1, "1.34", nullptr, 1, false, nullptr, false);
    s_assaoShadowClampCVar = CVarRegister("assaoShadowClamp", "ASSAO Shadow Clamp [0.0, 1.0]", 1, ".98", nullptr, 1, false, nullptr, false);
    s_assaoHorizonAngleThreshCVar = CVarRegister("assaoHorizonAngleThresh", "ASSAO Horizon Angle Thresh [0.0, 0.2] Limits self-shadowing", 1, "0.4", nullptr, 1, false, nullptr, false);
    s_assaoFadeOutFromCVar = CVarRegister("assaoFadeOutFrom", "ASSAO Fade Out From [0.0, ~ ] Distance to start start fading out the effect.", 1, "50.0", nullptr, 1, false, nullptr, false);
    s_assaoFadeOutToCVar = CVarRegister("assaoFadeOutTo", "ASSAO Fade Out To [0.0, ~ ] Distance at which the effect is faded out.", 1, "300.0", nullptr, 1, false, nullptr, false);
    s_assaoAdaptiveQualityLimitCVar = CVarRegister("assaoAdaptiveQualityLimit", "ASSAO Adaptive Quality Limit [0.0, 1.0] (only for Quality Level 3)", 1, ".45", nullptr, 1, false, nullptr, false);
    s_assaoBlurPassCountCVar = CVarRegister("assaoBlurPassCount", "ASSAO Blur Pass Count [ 0, 6] Number of edge-sensitive smart blur passes to apply. Quality 0 is an exception with only one 'dumb' blur pass used.", 1, "2", nullptr, 1, false, nullptr, false);
    s_assaoSharpnessCVar = CVarRegister("assaoSharpness", "ASSAO Sharpness [0.0, 1.0] (How much to bleed over edges; 1: not at all, 0.5: half-half; 0.0: completely ignore edges)", 1, ".98", nullptr, 1, false, nullptr, false);
    s_assaoTemporalSSAngleOffsetCVar = CVarRegister("assaoTemporalSSAngleOffset", "ASSAO Temporal Super Sampling Angle Offset [0.0, PI] Used to rotate sampling kernel; If using temporal AA / supersampling, suggested to rotate by ( (frame%3)/3.0*PI ) or similar. Kernel is already symmetrical, which is why we use PI and not 2*PI.", 1, "0.0", nullptr, 1, false, nullptr, false);
    s_assaoTemporalSSRadiusOffsetCVar = CVarRegister("assaoTemporalSSRadiusOffset", "ASSAO Temporal Super Sampling Radius Offset [0.0, 2.0] Used to scale sampling kernel; If using temporal AA / supersampling, suggested to scale by ( 1.0f + (((frame%3)-1.0)/3.0)*0.1 ) or similar.", 1, "1.0", nullptr, 1, false, nullptr, false);
    s_assaoDetailShadowStrengthCVar = CVarRegister("assaoDetailShadowStrength", "ASSAO Detail Shadow Strength [0.0, 5.0] Used for high-res detail AO using neighboring depth pixels: adds a lot of detail but also reduces temporal stability (adds aliasing).", 1, "0", nullptr, 1, false, nullptr, false);
    s_cacaoBilateralSimilarityDistanceSigmaCVar = CVarRegister("cacaoBilateralSimilarityDistanceSigma", "CACAO Sigma squared value for use in bilateral upsampler giving similarity weighting for neighbouring pixels. Should be greater than 0.0.", 1, "0.01", nullptr, 1, false, nullptr, false);
    s_ssaoMagicNormalsCVar = CVarRegister("ssaoMagicNormals", "SSAO Use combined GBuffer and face normals; attempts to get the best compromise for architecture, foliage, and characters", 1, "1", nullptr, 1, false, nullptr, false);
    s_ssaoMagicThresholdLowCVar = CVarRegister("ssaoMagicThresholdLow", "SSAO Low threshold for transitioning from gbuffer to face normal (degrees)", 1, "25", nullptr, 1, false, nullptr, false);
    s_ssaoMagicThresholdHighCVar = CVarRegister("ssaoMagicThresholdHigh", "SSAO High threshold for transitioning from gbuffer to face normal (degrees)", 1, "50", nullptr, 1, false, nullptr, false);
    s_resolvedSSAOTypeCVar = CVarRegister("ResolvedSSAOType", "", 0x1400, "0", ResolvedSSAOTypeCallback, 1, false, nullptr, false);
}
