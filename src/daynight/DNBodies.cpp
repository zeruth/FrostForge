#include "daynight/DayNight.hpp"

#include "base/Status.hpp"
#include "client/RenderSettings.hpp"
#include "gx/Gx.hpp"
#include "gx/GxPipeline.hpp"
#include "texture/Texture.hpp"
#include "util/Unimplemented.hpp"
#include <cmath>

namespace {

// The glare queries' shared pipeline and its users (DAT_1447aad40, DAT_1447aad48).
int32_t s_glareUsers;
GxPipeline* s_glarePipeline;

// The planets' four pipelines and their users (DAT_1447aad50, DAT_1447aad58..70).
int32_t s_planetUsers;
GxPipeline* s_planetPipelines[4];

// ref: FUN_142453a00
void GlareOnDeviceCreated(void* param) {
    (void)param;
    GXPIPELINEKEY key;
    std::memset(&key, 0, sizeof(key));
    key.m_topology = 4;
    key.m_samplesLog2 = g_renderSettings.m_msaaSamples;
    uint32_t raster = 0x10000;
    std::memcpy(key.m_raster, &raster, 4);
    uint64_t depthStencil = 0x700000011ull;
    std::memcpy(key.m_depthStencil, &depthStencil, 8);
    RENDERTARGETFORMATS formats = RenderTargetFormats(6);
    std::memcpy(key.m_targetFormats, &formats, 4);
    key.m_depthFormat = formats.m_depth;
    key.m_vertexShader = 0x39;
    key.m_pixelShader = 0x50;
    key.m_vertexFormat = 0xb;
    s_glarePipeline = GxPipelineCreate(&key, "DNGlareQuery");
}

// LAB_142453b50
void GlareOnDeviceDestroyed(void* param) {
    (void)param;
    GxPipelineDestroy(&s_glarePipeline);
}

// ref: FUN_142454550
void GlareOnRenderSettings(const RENDERSETTINGS* previous, const RENDERSETTINGS* current, void* param) {
    if (previous->m_sunshafts == current->m_sunshafts && previous->m_msaaSamples == current->m_msaaSamples) {
        return;
    }
    GxPipelineDestroy(&s_glarePipeline);
    GlareOnDeviceCreated(param);
}

// ref: FUN_142455560
void PlanetOnDeviceCreated(void* param) {
    (void)param;
    uint8_t samples = g_renderSettings.m_msaaSamples;
    GXPIPELINEKEY key;
    std::memset(&key, 0, sizeof(key));
    key.m_topology = 4;
    key.m_samplesLog2 = samples;
    uint32_t raster = 0x10000;
    std::memcpy(key.m_raster, &raster, 4);
    uint64_t depthStencil = 0x700000011ull;
    std::memcpy(key.m_depthStencil, &depthStencil, 8);
    RENDERTARGETFORMATS formats = RenderTargetFormats(6);
    std::memcpy(key.m_targetFormats, &formats, 4);
    key.m_depthFormat = formats.m_depth;
    key.m_vertexShader = 0x3a;
    key.m_pixelShader = 0x51;
    key.m_vertexFormat = 0xc;
    key.m_blend = 0xf002;
    s_planetPipelines[0] = GxPipelineCreate(&key, "DNPlanet");
    key.m_samplesLog2 = 0;
    s_planetPipelines[2] = GxPipelineCreate(&key, "DNPlanet Reflection");
    key.m_samplesLog2 = samples;
    key.m_vsPermute = 1;
    key.m_psPermute = 1;
    s_planetPipelines[1] = GxPipelineCreate(&key, "DNPlanet Light Texture");
    key.m_samplesLog2 = 0;
    s_planetPipelines[3] = GxPipelineCreate(&key, "DNPlanet Light Texture Reflection");
}

// ref: FUN_1424557a0
void PlanetOnDeviceDestroyed(void* param) {
    (void)param;
    for (GxPipeline*& pipeline : s_planetPipelines) {
        GxPipelineDestroy(&pipeline);
    }
}

// ref: FUN_142456000
void PlanetOnRenderSettings(const RENDERSETTINGS* previous, const RENDERSETTINGS* current, void* param) {
    if (previous->m_msaaSamples != current->m_msaaSamples) {
        for (GxPipeline*& pipeline : s_planetPipelines) {
            GxPipelineDestroy(&pipeline);
        }
        PlanetOnDeviceCreated(param);
    }
}

// The fields both glare constructors share (the base part of FUN_142453800/FUN_142453720).
void InitGlareBase(DNGlare* glare) {
    glare->Field<uint16_t>(0x14) = 0x100;
    glare->m_query = nullptr;
    glare->Field<uint32_t>(0x10) = 0;
    glare->Field<uint64_t>(0x18) = 0;
    glare->Field<uint64_t>(0x20) = 0;
    glare->m_texture = nullptr;
    std::memset(glare->m_30, 0, sizeof(glare->m_30));
}

}  // namespace

// ---- DNGlare -------------------------------------------------------------------------------

// ref: FUN_142453800
DNSunGlare::DNSunGlare() {
    InitGlareBase(this);
    this->Field<float>(0x44) = 1.0f;
    this->Field<float>(0x30) = 1.0f;
    this->Field<uint32_t>(0x3c) = 0x3fc1f07c;
    this->Field<float>(0x38) = 4.0f;
    this->Field<float>(0xb4) = 0.5f;
    this->Field<uint64_t>(0xb8) = 0x3f800000;
    this->Field<float>(0xa8) = 3.0f;
    this->Field<float>(0xac) = 20.0f;
    this->Field<uint32_t>(0xb0) = 0x3f333333;
    this->Field<uint32_t>(0x40) = 0;
    this->Field<uint8_t>(0x48) = 1;
    this->Field<uint64_t>(0x88) = 0x3e8aaaab;
    this->Field<uint32_t>(0x90) = 0x3ea00000;
    this->Field<float>(0x94) = 1.0f;
    this->Field<uint32_t>(0x98) = 0x3f500000;
    this->Field<float>(0x9c) = 1.0f;
    this->Field<uint64_t>(0xa0) = 0x3f600000;
}

// ref: FUN_142453720
DNMoonGlare::DNMoonGlare() {
    InitGlareBase(this);
    this->Field<float>(0x44) = 1.0f;
    this->Field<float>(0x30) = 2.0f;
    this->Field<uint32_t>(0x3c) = 0x3fc1f07c;
    this->Field<uint32_t>(0x38) = 0x4041f07c;
    this->Field<uint32_t>(0xb4) = 0x3dcccccd;
    this->Field<uint64_t>(0xb8) = 0x3f800000;
    this->Field<float>(0xa8) = 1.0f;
    this->Field<float>(0xac) = 1.0f;
    this->Field<uint32_t>(0xb0) = 0x3f333333;
    this->Field<uint32_t>(0x40) = 0;
    this->Field<uint8_t>(0x48) = 1;
    this->Field<uint32_t>(0x88) = 0x3daaaaab;
    this->Field<float>(0x8c) = 1.0f;
    this->Field<uint64_t>(0x90) = 0x3e0aaaab;
    this->Field<uint64_t>(0x98) = 0x3f72aaab;
    this->Field<uint32_t>(0xa0) = 0x3f7fd27d;
    this->Field<float>(0xa4) = 1.0f;
}

// ref: FUN_142453920
DNGlare::~DNGlare() {
    GxQueryDestroy(this->m_query);
    this->m_query = nullptr;
    this->Field<uint8_t>(0x14) = 0;
}

// ref: FUN_142454630
void DNGlare::Update(DNScene* scene) {
    (void)scene;
    FROST_UNIMPLEMENTED();
}

// ref: FUN_142454590
void DNGlare::Slot10() {
    FROST_UNIMPLEMENTED();
}

// ref: FUN_142453d00
uint32_t DNGlare::Visibility() {
    if (this->Field<uint8_t>(0x15) != 0 && this->Field<uint8_t>(0x27) != 0) {
        uint32_t samples = 0;
        if (this->m_query != nullptr) {
            FROST_UNIMPLEMENTED();  // FUN_142453d50: the query's visible samples
        }
        this->Field<uint32_t>(0xc4) = samples;
        return this->Field<uint32_t>(0xc0);
    }
    return 0;
}

// ref: FUN_142453ce0
uint8_t DNGlare::TextureLoaded() {
    return TextureIsReady(static_cast<HTEXTURE>(this->m_texture)) ? 1 : 0;
}

// ref: FUN_142454620
uint64_t DNGlare::SetTexturePriority(int32_t priority) {
    auto texture = static_cast<HTEXTURE>(this->m_texture);
    if (texture == nullptr) {
        return 0;
    }
    return TextureSetPriority(texture, priority) ? 1 : 0;
}

// ref: FUN_142453bc0
float DNSunGlare::CloudCover(DNScene* scene) {
    (void)scene;
    FROST_UNIMPLEMENTED();  // FUN_142450fb0: the cloud density under the body
    return 1.0f;
}

// ref: FUN_142453b60
float DNMoonGlare::CloudCover(DNScene* scene) {
    (void)scene;
    FROST_UNIMPLEMENTED();
    return 1.0f;
}

// ref: FUN_1424539c0
void DNGlare::CreateQuery() {
    if (GxDeviceCaps()[0xa8] != 0) {
        this->m_query = GxQueryCreate(0);
        GxQueryInfo(this->m_query, 0, &this->Field<uint32_t>(0x10));
    }
}

// ref: FUN_142453b20
void DNGlare::DestroyQuery() {
    GxQueryDestroy(this->m_query);
    this->m_query = nullptr;
    this->Field<uint8_t>(0x14) = 0;
}

// ref: FUN_142453c00
void DNGlare::Initialize(int32_t fileDataId, void* glareColor) {
    CStatus status;
    this->m_texture = TextureCreate(fileDataId, 0x82001, &status, 1, 1, 0);
    this->Field<uint64_t>(0x80) = 0;
    this->Field<uint8_t>(0x14) = 0;
    this->Field<uint64_t>(0xc0) = 0;
    if (s_glareUsers++ == 0) {
        GxAddDeviceCreatedCallback(GlareOnDeviceCreated, nullptr);
        GxAddDeviceDestroyedCallback(GlareOnDeviceDestroyed, nullptr);
        RenderSettingsAddHandler(GlareOnRenderSettings, nullptr);
    }
    this->Field<void*>(0xd0) = glareColor;
}

// ref: FUN_142453ac0
void DNGlare::Release() {
    if (this->m_texture != nullptr) {
        auto texture = static_cast<HTEXTURE>(this->m_texture);
        TextureFree(&texture);
        this->m_texture = texture;
    }
    if (--s_glareUsers == 0) {
        GxRemoveDeviceCreatedCallback(GlareOnDeviceCreated, nullptr);
        GxRemoveDeviceDestroyedCallback(GlareOnDeviceDestroyed, nullptr);
        RenderSettingsRemoveHandler(GlareOnRenderSettings, nullptr);
        GxPipelineDestroy(&s_glarePipeline);
    }
}

// ---- DNPLANET ------------------------------------------------------------------------------

// ref: FUN_142455150
DNPLANET::DNPLANET() {
    std::memset(this->m_data, 0, sizeof(this->m_data));
    this->Field<float>(0x58) = 1.0f;
    this->Field<float>(0x5c) = 1.0f;
    this->Field<float>(0x60) = 1.0f;
    this->Field<uint64_t>(0x64) = 0x3f800000;
    this->Field<float>(0xa0) = 12.0f;
}

// ref: FUN_142455a70
void DNPLANET::Initialize(int32_t fileDataId, float scale, float scale2) {
    CStatus status;
    this->Field<HTEXTURE>(0x10) = TextureCreate(fileDataId, 0x82001, &status, 0, 1, 0);
    if (s_planetUsers++ == 0) {
        GxAddDeviceCreatedCallback(PlanetOnDeviceCreated, nullptr);
        GxAddDeviceDestroyedCallback(PlanetOnDeviceDestroyed, nullptr);
        RenderSettingsAddHandler(PlanetOnRenderSettings, nullptr);
    }
    this->Field<float>(0x5c) = scale;
    this->Field<float>(0x60) = scale2;
}

// ref: FUN_142455680
void DNPLANET::Release() {
    for (size_t offset = 0x10; offset <= 0x48; offset += 8) {
        if (this->Field<HTEXTURE>(offset) != nullptr) {
            TextureFree(&this->Field<HTEXTURE>(offset));
        }
    }
    if (--s_planetUsers == 0) {
        GxRemoveDeviceCreatedCallback(PlanetOnDeviceCreated, nullptr);
        GxRemoveDeviceDestroyedCallback(PlanetOnDeviceDestroyed, nullptr);
        RenderSettingsRemoveHandler(PlanetOnRenderSettings, nullptr);
        for (GxPipeline*& pipeline : s_planetPipelines) {
            GxPipelineDestroy(&pipeline);
        }
    }
    TextureFree(&this->Field<HTEXTURE>(0x50));
}
