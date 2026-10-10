#include "daynight/DayNight.hpp"

#include "base/Status.hpp"
#include "client/RenderSettings.hpp"
#include "gx/Gx.hpp"
#include "gx/GxPipeline.hpp"
#include "gx/GxRenderTargets.hpp"
#include "texture/Texture.hpp"
#include <cmath>
#include <cstdlib>

namespace {

// DAT_1447aa438 and the three tables it guards: noise in (-1, 1], the cosine interpolation
// weights across a cell, and the density falloff as bytes.
bool s_tablesBuilt;
float s_noise[0x100];               // DAT_1447aa540
float s_cosine[0x100];              // DAT_1447aa940
uint8_t s_falloff[0x100];           // DAT_1447aa440

// The clouds' shared device objects and their users (DAT_1447aa418..DAT_1447aa430).
int32_t s_users;
GxPipeline* s_pipeline;             // "DNClouds"
GxPipeline* s_reflectionPipeline;   // "DNClouds Reflection"
HTEXTURE s_texture;                 // 0x354b9e

// The quality (DAT_1447aa410, a CVar's), the density override (DAT_1447aa414) and the cell
// count override (DAT_142d4d460).
uint8_t s_quality;
float s_densityOverride;
int32_t s_cellOverride = 8;

// The grid sizes and their log2 by quality (DAT_1429799b0, DAT_1429799c8).
const int32_t s_gridSizes[] = {0x80, 0x100, 0x200, 0x400, 0x800, 0};
const int32_t s_gridLog2[] = {7, 8, 9, 10, 11, 0};

// The dome's ring heights as fractions of a half turn (DAT_142979970) and each ring's alpha
// (DAT_1429799a0).
const float s_ringAngles[12] = {0.0f, 0.025f, 0.05f, 0.075f, 0.1f, 0.125f, 0.15f, 0.175f, 0.205f, 0.23f,
    0.245f, 0.25f};
const uint8_t s_ringAlpha[12] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 128, 0, 0};

constexpr float RAND_SCALE = 3.0518509e-05f;    // DAT_1427228e4, 1 / 32767
constexpr float CELL_ANGLE = 0.012271847f;      // DAT_142979a20, pi / 256
constexpr float DEFAULT_DENSITY = 0.6f;         // DAT_142618d60
constexpr float FALLOFF_BASE = 0.96f;           // DAT_142737e20
constexpr float FALLOFF_STEP = 0.00390625f;     // DAT_1426c6fe0
constexpr float PI = 3.1415927f;                // DAT_1426c9758
constexpr float TWO_PI = 6.2831855f;            // DAT_1426c70ac
constexpr float RING_STEP = 0.3926991f;         // DAT_1426d0ff0, pi / 8
constexpr float HORIZON_DROP = 0.70710677f;     // DAT_1426d2508
constexpr float TEX_SCALE = 0.045454547f;       // DAT_142979a04, 1 / 22
constexpr float EPSILON = 9.536743e-07f;        // DAT_14261a958

// ref: FUN_1424508f0
void OnDeviceCreated(void* param) {
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
    key.m_vertexShader = 0x38;
    key.m_pixelShader = 0x4f;
    key.m_vertexFormat = 0xc;
    key.m_blend = 0xf002;
    s_pipeline = GxPipelineCreate(&key, "DNClouds");
    key.m_samplesLog2 = 0;
    s_reflectionPipeline = GxPipelineCreate(&key, "DNClouds Reflection");
    CStatus status;
    s_texture = TextureCreateById(0x354b9e, &status);
}

// ref: FUN_142450a70
void OnDeviceDestroyed(void* param) {
    (void)param;
    GxPipelineDestroy(&s_pipeline);
    GxPipelineDestroy(&s_reflectionPipeline);
    TextureFree(&s_texture);
}

// ref: FUN_142451270
void OnRenderSettings(const RENDERSETTINGS* previous, const RENDERSETTINGS* current, void* param) {
    if (previous->m_msaaSamples != current->m_msaaSamples) {
        GxPipelineDestroy(&s_pipeline);
        GxPipelineDestroy(&s_reflectionPipeline);
        TextureFree(&s_texture);
        OnDeviceCreated(param);
    }
}

}  // namespace

// ref: FUN_142450700
DNCLOUDS::DNCLOUDS() {
    this->m_enabled = 1;
    this->m_pad3c = 0;
    this->m_104 = 0;
    this->m_0a = 0;
    this->m_falloff = 0;
    this->m_quality = 0;
    this->m_1c = 0;
    this->m_20 = 0;
    this->m_34 = 4;
    this->m_24 = 2;
    this->m_10 = 0.002f;    // 0x3b03126f
    this->m_dirty = 1;
    this->m_08 = 0;
    this->m_00 = 1.0f;
    this->m_cells = 8;
    this->m_18 = -1;
    this->m_gridSize = 0x80;
    this->m_gridMask = 0x7f;
    this->m_gridLog2 = 7;
    this->m_indexCount = 0;
    this->m_vertexCount = 0;
}

// ref: FUN_1424507f0
DNCLOUDS::~DNCLOUDS() {
    this->m_indices.Free();
    this->m_vertexColors.Free();
    this->m_texCoords.Free();
    this->m_positions.Free();
    this->m_88.Free();
    this->m_density.Free();
    // m_colors and m_textures (FUN_14129eff0 destroys each texture) free as blz::vector does.
    for (void*& tex : this->m_textures) {
        if (tex != nullptr) {
            GxTexDestroy(&tex);
        }
    }
}

// ref: FUN_142451030
void DNCLOUDS::Initialize() {
    if (!s_tablesBuilt) {
        for (float& noise : s_noise) {
            float scaled = static_cast<float>(std::rand()) * RAND_SCALE;
            noise = 1.0f - (scaled + scaled);
        }
        for (int32_t i = 0; i < 0x100; i++) {
            s_cosine[i] = (1.0f - std::cos(static_cast<float>(i) * CELL_ANGLE)) * 0.5f;
        }
        s_tablesBuilt = true;
    }
    if (s_users++ == 0) {
        GxAddDeviceCreatedCallback(OnDeviceCreated, nullptr);
        GxAddDeviceDestroyedCallback(OnDeviceDestroyed, nullptr);
        RenderSettingsAddHandler(OnRenderSettings, nullptr);
    }
    this->BuildGrid();
    this->BuildDome(1.0f);
    float density = s_densityOverride != 0.0f ? s_densityOverride : DEFAULT_DENSITY;
    density = 1.0f - density;
    if (density < 0.0f) {
        density = 0.0f;
    } else if (density > 1.0f) {
        density = 1.0f;
    }
    uint8_t falloff = static_cast<uint8_t>(static_cast<int32_t>(density * 255.0f));
    if (falloff != this->m_falloff && falloff == 0xff) {
        this->m_05 = 100;
    }
    this->m_falloff = falloff;
    this->m_00 = 0.96f;     // 0x3f75c28f
    float exponent = 0.0f;
    float step = static_cast<float>(0xff - falloff) * FALLOFF_STEP;
    for (uint8_t& value : s_falloff) {
        float scaled = std::pow(FALLOFF_BASE, exponent) * 255.0f;
        exponent += step;
        value = static_cast<uint8_t>(static_cast<int32_t>(255.0f - scaled));
    }
    this->m_enabled = 1;
    this->m_20 = 0;
}

// ref: FUN_142452b80
void DNCLOUDS::BuildGrid() {
    if (this->m_textures.size() != 0) {
        this->DestroyTextures();
        this->m_density.Free();
        this->m_density.m_alloc = 0;
        this->m_density.m_count = 0;
        this->m_density.m_data = nullptr;
        this->m_88.Free();
        this->m_88.m_alloc = 0;
        this->m_88.m_count = 0;
        this->m_88.m_data = nullptr;
    }
    this->m_quality = s_quality;
    int32_t gridSize = s_gridSizes[s_quality];
    this->m_gridSize = gridSize;
    this->m_cells = s_cellOverride != 0 ? s_cellOverride : 8;
    this->m_gridMask = gridSize - 1;
    this->m_gridLog2 = s_gridLog2[s_quality];
    this->m_colors.m_size = 0;
    if (gridSize * gridSize != 0) {
        this->m_colors.resize(static_cast<uint64_t>(gridSize * gridSize));
        for (uint32_t& color : this->m_colors) {
            color = 0xff;
        }
    }
    uint32_t cells = static_cast<uint32_t>(this->m_gridSize * this->m_gridSize);
    if (cells != this->m_density.m_count) {
        if (cells == 0) {
            this->m_density.Free();
            this->m_density.m_alloc = 0;
            this->m_density.m_count = 0;
            this->m_density.m_data = nullptr;
        } else {
            this->m_density.ReallocData(cells);
            this->m_density.m_count = cells;
        }
    }
    uint32_t columns = static_cast<uint32_t>(this->m_gridSize);
    if (columns != this->m_88.m_count) {
        if (columns == 0) {
            this->m_88.Free();
            this->m_88.m_alloc = 0;
            this->m_88.m_count = 0;
            this->m_88.m_data = nullptr;
        } else {
            this->m_88.ReallocData(columns);
            this->m_88.m_count = columns;
        }
    }
    std::memset(this->m_88.m_data, 0, uint64_t(this->m_88.m_count) * sizeof(float));
    this->m_1c = 0;
    this->m_dirty = 1;
}

// ref: FUN_142450aa0
void DNCLOUDS::BuildDome(float radius) {
    this->m_positions.SetCount(0xc0);
    this->m_texCoords.SetCount(0xc0);
    this->m_vertexColors.SetCount(0xc0);
    if (this->m_indices.m_count != 0x176) {
        this->m_indices.ReallocData(0x176);
        this->m_indices.m_count = 0x176;
    }
    DNVEC3* position = this->m_positions.m_data;
    DNVEC2* texCoord = this->m_texCoords.m_data;
    uint32_t* color = this->m_vertexColors.m_data;
    uint16_t* index = this->m_indices.m_data;
    uint32_t previousStart = 0;
    float previousAngle = 0.0f;
    for (uint32_t ring = 0; ring < 12; ring++) {
        float angle = s_ringAngles[ring] * PI;
        float ringRadius = std::sin(angle) * radius;
        float height = std::cos(angle) * radius - HORIZON_DROP;
        uint32_t start = static_cast<uint32_t>(position - this->m_positions.m_data);
        float around = 0.0f;
        for (int32_t i = 0; i < 0x10; i++) {
            float s = std::sin(around);
            float c = std::cos(around);
            position->x = ringRadius * s;
            position->y = ringRadius * c;
            position->z = height;
            position++;
            *color++ = 0x00ffffffu | (uint32_t(s_ringAlpha[ring]) << 24);
            texCoord->x = s * static_cast<float>(ring) * TEX_SCALE + 0.5f;
            texCoord->y = c * static_cast<float>(ring) * TEX_SCALE + 0.5f;
            texCoord++;
            if (std::fabs(angle) < EPSILON || std::fabs(angle - TWO_PI) < EPSILON) {
                break;
            }
            around += RING_STEP;
        }
        if (ring != 0) {
            for (int32_t i = 0; i < 0x11; i++) {
                uint16_t step = static_cast<uint16_t>(i & 0xf);
                index[0] = static_cast<uint16_t>((EPSILON <= std::fabs(previousAngle) ? step : 0) + previousStart);
                index[1] = static_cast<uint16_t>((EPSILON <= std::fabs(angle - TWO_PI) ? step : 0) + start);
                index += 2;
            }
        }
        previousStart = start;
        previousAngle = angle;
    }
    this->m_indexCount = 0x176;
    this->m_vertexCount = static_cast<uint16_t>(position - this->m_positions.m_data);
}

// ref: FUN_142451380
void DNCLOUDS::DestroyTextures() {
    for (void* tex : this->m_textures) {
        if (tex != nullptr) {
            void* local = tex;
            GxTexDestroy(&local);
        }
    }
    this->m_textures.m_size = 0;
}

// ref: FUN_142450a00
void DNCLOUDS::Release() {
    this->DestroyTextures();
    if (--s_users == 0) {
        GxRemoveDeviceCreatedCallback(OnDeviceCreated, nullptr);
        GxRemoveDeviceDestroyedCallback(OnDeviceDestroyed, nullptr);
        RenderSettingsRemoveHandler(OnRenderSettings, nullptr);
        GxPipelineDestroy(&s_pipeline);
        GxPipelineDestroy(&s_reflectionPipeline);
        TextureFree(&s_texture);
    }
}

namespace {

// The grid's rows as the upload reads them (the reference's generic reader, LAB_1405e13c0).
struct ROWSOURCE {
    uint32_t m_pitch;
    uint32_t m_pad04;
    const void* m_data;
};

// LAB_1405e13c0
void ReadRows(void* param, uint32_t width, uint32_t height, uint32_t slice, uint32_t mip, uint32_t* pitch,
    const void** data) {
    (void)width;
    (void)height;
    (void)slice;
    (void)mip;
    auto source = static_cast<ROWSOURCE*>(param);
    *pitch = source->m_pitch;
    *data = source->m_data;
}

// A GxTex's size and whether it is ready (its ushorts at +0 and +2, bit 0 of +0x10).
uint16_t TexWidth(void* tex) {
    return static_cast<uint16_t*>(tex)[0];
}

uint16_t TexHeight(void* tex) {
    return static_cast<uint16_t*>(tex)[1];
}

bool TexReady(void* tex) {
    return (static_cast<uint16_t*>(tex)[8] & 1) != 0;
}

// FUN_1424520f0: the whole grid into one texture.
// ref: FUN_1424520f0
void UploadWhole(GxRenderContext* context, void* tex, uint32_t pitch, const void* data) {
    int32_t rect[4] = {0, 0, TexHeight(tex), TexWidth(tex)};
    ROWSOURCE source = {pitch, 0, data};
    GxTexUpdateInFrame(context, tex, rect, false, ReadRows, &source);
}

}  // namespace

// ref: FUN_142452f30
void DNCLOUDS::Upload(DNScene* scene) {
    GxRenderContext* context = g_gxRenderContext;
    if (this->m_textures.size() == 0) {
        uint32_t frames = GxDeviceBufferedFrames();
        this->m_104 = frames;
        if (frames == 1) {
            frames = 2;
        }
        // FUN_142453320: blz::vector<unique_ptr<GxTex>>::resize.
        this->m_textures.resize(frames);
        for (uint64_t i = 0; i < this->m_textures.size(); i++) {
            void* tex = nullptr;
            bool created = GxTexCreateParams(0, static_cast<uint32_t>(this->m_gridSize),
                static_cast<uint32_t>(this->m_gridSize), 1, this->m_24, 2, 0x10082001, nullptr, &tex);
            void* old = this->m_textures[i];
            this->m_textures[i] = tex;
            if (old != nullptr) {
                GxTexDestroy(&old);
            }
            void* current = this->m_textures[i];
            if (created && TexReady(current)) {
                ROWSOURCE source = {static_cast<uint32_t>(this->m_gridSize << 2), 0, this->m_colors.data()};
                int32_t rect[4] = {0, 0, TexHeight(current), TexWidth(current)};
                GxTexUpdateInFrame(context, current, rect, false, ReadRows, &source);
            }
        }
    } else if (this->m_104 < 2) {
        void* tex = this->m_textures[static_cast<uint8_t>(this->m_08 - 1) & 1];
        if (tex != nullptr && TexReady(tex)) {
            int32_t rect[4] = {0, static_cast<int32_t>(this->m_1c), this->m_gridSize,
                static_cast<int32_t>(this->m_1c + static_cast<uint32_t>(this->m_cells))};
            ROWSOURCE source = {static_cast<uint32_t>(this->m_gridSize << 2), 0, this->m_colors.data()};
            GxTexUpdateInFrame(context, tex, rect, false, ReadRows, &source);
        }
    } else {
        void* tex = this->m_textures[GxDeviceFrameSlot()];
        if (tex != nullptr && TexReady(tex)) {
            UploadWhole(context, tex, static_cast<uint32_t>(this->m_gridSize << 2), this->m_colors.data());
        }
    }
    uint32_t column = this->m_20 + this->m_1c;
    this->m_1c = column;
    if (column < static_cast<uint32_t>(this->m_gridSize)) {
        this->m_20 = 0;
        return;
    }
    auto pass = static_cast<int16_t>(static_cast<int32_t>(
        static_cast<float>(scene->m_light.Field<uint32_t>(0x50)) * this->m_10));
    if (pass == static_cast<int16_t>(this->m_0a)) {
        if (this->m_dirty == 0) {
            this->m_1c = 0;
            this->m_20 = 0;
            return;
        }
    } else {
        this->m_0a = static_cast<uint16_t>(pass);
    }
    this->m_08 = static_cast<uint8_t>((this->m_08 - 1) & 1);
    this->m_1c = 0;
    this->m_20 = 0;
}
