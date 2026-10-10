#include "daynight/DayNight.hpp"

#include "base/Matrix.hpp"
#include "base/Status.hpp"
#include "client/RenderSettings.hpp"
#include "gx/Gx.hpp"
#include "gx/GxPipeline.hpp"
#include "texture/Texture.hpp"
#include <cmath>

namespace {

// The sky's shared device objects and their users (DAT_1447aad78..DAT_1447aada0).
int32_t s_users;
GxPipeline* s_pipeline;             // "DNSky"
GxPipeline* s_reflectionPipeline;   // "DNSky Reflection"
GxPipeline* s_fogPipeline;          // "DNSky Fog"
GxPipeline* s_fogReflectionPipeline;    // "DNSky Fog Reflection"
HTEXTURE s_texture;                 // 0x354b9e

// The dome's bands as fractions of a turn from the zenith (DAT_142d4d6e0); the sixth is also
// how far down the dome is lifted.
float s_bands[7] = {0.0f, 0.17f, 0.2f, 0.23f, 0.235f, 0.25f, 1.0f};

constexpr float PI = 3.1415927f;            // DAT_1426c9758
constexpr float SEGMENT = 0.2617994f;       // DAT_1427dd95c, pi / 12
constexpr float HORIZON_Z = -0.005f;        // DAT_1427b0174
constexpr float EPSILON = 9.536743e-07f;    // DAT_14261a958

// ref: FUN_142456c30
void OnDeviceCreated(void* param) {
    (void)param;
    GXPIPELINEKEY key;
    std::memset(&key, 0, sizeof(key));
    key.m_topology = 4;
    uint32_t raster = 0x10000;
    std::memcpy(key.m_raster, &raster, 4);
    uint64_t depthStencil = 0x700000011ull;
    std::memcpy(key.m_depthStencil, &depthStencil, 8);
    RENDERTARGETFORMATS formats = RenderTargetFormats(6);
    std::memcpy(key.m_targetFormats, &formats, 4);
    key.m_depthFormat = formats.m_depth;
    key.m_vertexShader = 0x3b;
    key.m_pixelShader = 0x52;
    key.m_vertexFormat = 0xb;
    key.m_blend = 0xf003;
    key.m_samplesLog2 = 0;
    s_reflectionPipeline = GxPipelineCreate(&key, "DNSky Reflection");
    key.m_samplesLog2 = g_renderSettings.m_msaaSamples;
    s_pipeline = GxPipelineCreate(&key, "DNSky");
    key.m_blend = 0xf002;
    s_fogPipeline = GxPipelineCreate(&key, "DNSky Fog");
    key.m_samplesLog2 = 0;
    s_fogReflectionPipeline = GxPipelineCreate(&key, "DNSky Fog Reflection");
    CStatus status;
    s_texture = TextureCreateById(0x354b9e, &status);
}

// ref: FUN_142456e00
void OnDeviceDestroyed(void* param) {
    (void)param;
    GxPipelineDestroy(&s_pipeline);
    GxPipelineDestroy(&s_reflectionPipeline);
    GxPipelineDestroy(&s_fogPipeline);
    GxPipelineDestroy(&s_fogReflectionPipeline);
    TextureFree(&s_texture);
}

// ref: FUN_142457230
void OnRenderSettings(const RENDERSETTINGS* previous, const RENDERSETTINGS* current, void* param) {
    if (previous->m_msaaSamples != current->m_msaaSamples) {
        OnDeviceDestroyed(param);
        OnDeviceCreated(param);
    }
}

}  // namespace

// ref: FUN_142456b70
DNSKY::DNSKY() {
    this->m_rings = 0;
    this->m_vertexCount = 0;
    this->m_indexCount = 0;
    std::memset(this->m_pad68, 0, sizeof(this->m_pad68));
}

// ref: FUN_142456ba0
DNSKY::~DNSKY() {
    this->m_indices.Free();
    this->m_colors2.Free();
    this->m_colors.Free();
    this->m_positions.Free();
}

// ref: FUN_1424571d0
void DNSKY::Initialize() {
    if (s_users++ == 0) {
        GxAddDeviceCreatedCallback(OnDeviceCreated, nullptr);
        GxAddDeviceDestroyedCallback(OnDeviceDestroyed, nullptr);
        RenderSettingsAddHandler(OnRenderSettings, nullptr);
    }
    this->Build(1.0f);
}

// ref: FUN_142456e50
void DNSKY::Build(float radius) {
    this->m_rings = 0x18;
    this->m_positions.SetCount(0xa8);
    this->m_colors.SetCount(0xa8);
    this->m_colors2.SetCount(0xa8);
    if (this->m_indices.m_count != 300) {
        this->m_indices.ReallocData(300);
        this->m_indices.m_count = 300;
    }
    DNVEC3* position = this->m_positions.m_data;
    uint16_t* index = this->m_indices.m_data;
    float lift = std::cos(s_bands[5] * PI);
    uint32_t previousStart = 0;
    float previousAngle = 0.0f;
    for (int32_t band = 0; band < 7; band++) {
        float angle = s_bands[band] * PI;
        float c = FastCos(angle);
        float s = FastSin(angle);
        uint32_t start = static_cast<uint32_t>(position - this->m_positions.m_data);
        float z = band == 5 ? HORIZON_Z : c * radius - lift;
        for (int32_t i = 0; i < 0x18; i++) {
            float around = static_cast<float>(i) * SEGMENT;
            position->z = z;
            position->y = std::cos(around) * s * radius;
            position->x = std::sin(around) * s * radius;
            position++;
            if (std::fabs(angle) < EPSILON || std::fabs(angle - PI) < EPSILON) {
                break;
            }
        }
        if (band > 0) {
            for (uint32_t u = 0; u < 0x19; u++) {
                uint16_t a = EPSILON <= std::fabs(previousAngle) ? static_cast<uint16_t>(u % 0x18) : 0;
                uint16_t b = EPSILON <= std::fabs(angle - PI) ? static_cast<uint16_t>(u - (u / 0x18) * 0x18) : 0;
                index[0] = static_cast<uint16_t>(a + previousStart);
                index[1] = static_cast<uint16_t>(b + start);
                index += 2;
            }
        }
        previousStart = start;
        previousAngle = angle;
    }
    this->m_indexCount = 300;
    this->m_vertexCount = static_cast<uint16_t>(position - this->m_positions.m_data);
}

// ref: FUN_142456d80
void DNSKY::Release() {
    if (--s_users == 0) {
        GxRemoveDeviceCreatedCallback(OnDeviceCreated, nullptr);
        GxRemoveDeviceDestroyedCallback(OnDeviceDestroyed, nullptr);
        RenderSettingsRemoveHandler(OnRenderSettings, nullptr);
        OnDeviceDestroyed(nullptr);
    }
}

namespace {

// How strongly the dome's bands lean toward the zenith colour over the day (DAT_142d4d700),
// and the shading round the azimuth: lit toward the sun, darker away (DAT_142d4d730).
const float s_mixBand[] = {0.125f, 0.0f, 0.27083334f, 1.0f, 0.29166666f, 0.0f, 0.85416669f, 0.0f, 0.89583331f,
    1.0f, 0.99930555f, 0.0f};
const float s_azimuthBand[] = {0.125f, 1.0f, 0.375f, 0.0f, 0.5f, -0.5f, 0.625f, -0.7f, 0.75f, -0.5f, 0.875f, 0.0f};

constexpr float TURN = 0.15915494f;         // DAT_142726830, 1 / 2pi
constexpr float SHADE_TOWARD_ZENITH = 0.7f; // DAT_1426c720c

uint32_t Tinted(uint32_t color, uint8_t tint, uint32_t tintColor, uint8_t alpha) {
    if (tint != 0) {
        ColorLerpByte(&color, tint, tintColor);
    }
    return (color & 0xffffff) | (static_cast<uint32_t>(alpha) << 24);
}

}  // namespace

// ref: FUN_14244c440
uint32_t DNColorLerp(const uint32_t* from, const uint32_t* to, float t) {
    uint32_t result = 0xff000000;
    for (uint32_t shift = 0; shift < 24; shift += 8) {
        auto a = static_cast<float>((*from >> shift) & 0xff);
        auto b = static_cast<float>((*to >> shift) & 0xff);
        result |= (static_cast<uint32_t>(static_cast<int32_t>((b - a) * t + a)) & 0xff) << shift;
    }
    return result;
}

// ref: FUN_142457d40
void DNSKY::Compute(DNCURRENTLIGHT* light, DNCURRENTFOG* fog, float time, float azimuth, uint8_t tint,
    uint32_t tintColor, float underwater, float opacity, float farClip) {
    auto alpha = static_cast<uint8_t>(static_cast<int32_t>(opacity * 255.0f));
    uint32_t* colors = this->m_colors.m_data;
    uint32_t* fogColors = this->m_colors2.m_data;
    float fogT = farClip / (*fog)[0x16];
    fogT = std::min(fogT, 1.0f);
    fogT = std::max(fogT, 0.0f);
    int32_t fogAlpha = 0xff;
    if (underwater <= 0.1f) {               // DAT_14261a97c
        fogAlpha = static_cast<int32_t>(underwater * 2550.0f);     // DAT_142979b90
    }
    uint32_t fogColor = DNColorLerp(&light->Word(0xa), &light->Word(0xb), fogT) & 0xffffff;
    float mix = DNFloatBand(s_mixBand, 6, time) * light->Float(0x18);
    uint32_t bands[9];
    for (uint32_t i = 0; i < 9; i++) {
        bands[i] = DNColorLerp(&light->Word(6 + i), &light->Word(6), mix);
    }
    *colors++ = (light->Word(5) & 0xffffff) | (static_cast<uint32_t>(alpha) << 24);
    *fogColors++ = fogColor;
    float step = -1.0f / static_cast<float>(this->m_rings);
    float turn = azimuth * TURN;
    for (uint32_t k = 0; k < 4; k++) {
        float at = turn + 0.25f;            // DAT_1426193c4
        if (0.75f < turn) {                 // DAT_14261a994
            at = turn - 0.75f;
        }
        for (int32_t j = 0; j < this->m_rings; j++) {
            if (at < 0.0f) {
                at += 1.0f;
            }
            float shade = DNFloatBand(s_azimuthBand, 6, at);
            uint32_t color;
            if (0.0f <= shade) {
                color = DNColorLerp(&light->Word(6 + k), &bands[k], (1.0f - shade) * mix);
            } else {
                uint32_t toward = DNColorLerp(&bands[k], &light->Word(5), mix * SHADE_TOWARD_ZENITH);
                color = DNColorLerp(&bands[k], &toward, -(shade * mix));
            }
            *colors++ = Tinted(color, tint, tintColor, alpha);
            at += step;
            *fogColors++ = fogColor;
        }
    }
    fogColor = (fogColor & 0xffffff) | ((static_cast<uint32_t>(fogAlpha) & 0xff) << 24);
    uint32_t horizon = Tinted(light->Word(0xb), tint, tintColor, alpha);
    for (int32_t j = 0; j < this->m_rings; j++) {
        *colors++ = horizon;
        *fogColors++ = fogColor;
    }
    *colors = Tinted(light->Word(0xb), tint, tintColor, alpha);
    *fogColors = fogColor;
}
