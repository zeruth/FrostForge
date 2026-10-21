#include "daynight/DayNight.hpp"

#include "base/Matrix.hpp"
#include "base/Status.hpp"
#include "client/RenderSettings.hpp"
#include "gx/Gx.hpp"
#include "gx/GxPipeline.hpp"
#include "gx/GxRenderTargets.hpp"
#include "model2/M2Model.hpp"
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

namespace {

// The noise permutation (DAT_142d4d4b0).
const uint8_t s_permutation[0x100] = {225, 155, 210, 108, 175, 199, 221, 144, 203, 116, 70, 213, 69, 158, 33, 252,
    5, 82, 173, 133, 222, 139, 174, 27, 9, 71, 90, 246, 75, 130, 91, 191, 169, 138, 2, 151, 194, 235, 81, 7, 25,
    113, 228, 159, 205, 253, 134, 142, 248, 65, 224, 217, 22, 121, 229, 63, 89, 103, 96, 104, 156, 17, 201, 129,
    36, 8, 165, 110, 237, 117, 231, 56, 132, 211, 152, 20, 181, 111, 239, 218, 170, 163, 51, 172, 157, 47, 80,
    212, 176, 250, 87, 49, 99, 242, 136, 189, 162, 115, 44, 43, 124, 94, 150, 16, 141, 247, 32, 10, 198, 223,
    255, 72, 53, 131, 84, 57, 220, 197, 58, 50, 208, 11, 241, 28, 3, 192, 62, 202, 18, 215, 153, 24, 76, 41, 15,
    179, 39, 46, 55, 6, 128, 167, 23, 188, 106, 34, 187, 140, 164, 73, 112, 182, 244, 195, 227, 13, 35, 77, 196,
    185, 26, 200, 226, 119, 31, 123, 168, 125, 249, 68, 183, 230, 177, 135, 160, 180, 12, 1, 243, 148, 102, 166,
    38, 238, 251, 37, 240, 126, 64, 74, 161, 40, 184, 149, 171, 178, 101, 66, 29, 59, 146, 61, 254, 107, 42, 86,
    154, 4, 236, 232, 120, 21, 233, 209, 45, 98, 193, 114, 78, 19, 206, 14, 118, 127, 48, 79, 147, 85, 30, 207,
    219, 54, 88, 234, 190, 122, 95, 67, 143, 109, 137, 214, 145, 93, 92, 100, 245, 0, 216, 186, 60, 83, 105, 97,
    204, 52};

// The octaves' steps across the grid by quality (five per quality, the stack table the
// generator builds).
const uint16_t s_octaveSteps[25] = {0x10, 0x20, 0x40, 0x80, 0x100, 0x08, 0x10, 0x20, 0x40, 0x80, 0x04, 0x08, 0x10,
    0x20, 0x40, 0x02, 0x04, 0x08, 0x10, 0x20, 0x01, 0x02, 0x04, 0x08, 0x04};

// How much the sun lights the clouds over the day (DAT_142d4d470).
const float s_lightBand[16] = {0.16666667f, 1.0f, 0.19444445f, 1.0f, 0.20138890f, 1.0f, 0.22916667f, 1.0f,
    0.89583331f, 1.0f, 0.92361110f, 1.0f, 0.88888890f, 1.0f, 0.91666669f, 1.0f};

// One octave's walk over the grid (0x54 bytes on the reference's stack): the 8.8 coordinates
// across the row, down the rows and through time, the hashed corners of the current cell and
// its noise values (cached until the column's cell changes).
struct OCTAVE {
    uint16_t m_x;           // 0x00
    uint16_t m_y;           // 0x02
    uint16_t m_z;           // 0x04
    uint16_t m_xStep;       // 0x06
    uint16_t m_yStep;       // 0x08
    uint16_t m_0a;
    uint16_t m_zInt;        // 0x0c
    uint16_t m_yInt;        // 0x0e
    uint16_t m_zInt2;       // 0x10
    uint16_t m_12;          // 0x12
    uint16_t m_yNext;       // 0x14
    uint16_t m_zNext;       // 0x16
    float m_amplitude;      // 0x18
    uint32_t m_corners[4];  // 0x1c
    float m_noise[8];       // 0x2c base and slope for each corner pair
    uint32_t m_xInt;        // 0x4c
    uint32_t m_cachedX;     // 0x50
};
static_assert(sizeof(OCTAVE) == 0x54, "an octave is 0x54 bytes");

// The fast inverse square root the generator inlines.
float FastInverseSqrt(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    bits = 0x5f3997bb - ((bits >> 1) & 0x3fffffff);
    float result;
    std::memcpy(&result, &bits, 4);
    return result;
}

// Where the line from the camera toward the light body crosses the cloud sky, as grid
// coordinates (FUN_1424531e0).
// ref: FUN_1424531e0
void CloudGridPoint(DNCLOUDS* clouds, const float* camera, const float* point, float* out) {
    float dx = point[0] - camera[0];
    float dy = point[1] - camera[1];
    float dz = point[2] - camera[2] - -0.70710677f;     // DAT_1427fe368
    float planar = dy * dy + dx * dx;
    float angle = std::acos(dz / (std::sqrt(dz * dz + planar) * std::sqrt(1.0f)));
    angle = std::min(angle, 0.78539819f);               // DAT_1427e0340
    angle *= 0.63661975f;                               // DAT_1427fb798
    float length = std::sqrt(planar);
    if (static_cast<double>(length) > 1e-05) {          // DAT_1428b46a0
        float scale = 1.0f / length;
        dx *= scale;
        dy *= scale;
    } else {
        dx = 0.0f;
        dy = 0.0f;
    }
    out[0] = (dx * angle + 0.5f) * static_cast<float>(static_cast<uint32_t>(clouds->m_gridSize));
    out[1] = (dy * angle + 0.5f) * static_cast<float>(static_cast<uint32_t>(clouds->m_gridSize));
}

// The colours the generator lights the clouds with: the light body's position on the grid
// (and its height), the sky's three cloud colours, and the light's strength over the day.
// ref: FUN_142451e30
void CloudLighting(DNCLOUDS* clouds, DNScene* scene, float* lit, float* direct, float* shadow, float* sun,
    float* strength) {
    float time = DNField<float>(scene, 0xc);
    const float* body = time < 0.20138890f || 0.92361110f < time   // DAT_142979a08, DAT_142979a0c
        ? &DNField<float>(scene, 0x1980)
        : &DNField<float>(scene, 0x18d8);
    const float* camera = &DNField<float>(scene, 0x14);
    float dx = body[0] - camera[0];
    float dy = body[1] - camera[1];
    float dz = body[2] - camera[2];
    float near = 0.0f;
    float far = 0.0f;
    QuadraticRoots(dy * dy + dx * dx + dz * dz, dz * 1.4142135f, -0.5f, &near, &far);
    float point[3] = {dx * far + camera[0], dy * far + camera[1], dz * far + camera[2]};
    float grid[2] = {0.0f, 0.0f};
    CloudGridPoint(clouds, camera, point, grid);
    sun[0] = grid[0];
    sun[1] = grid[1];
    sun[2] = 64.0f;
    auto bytes = reinterpret_cast<const uint8_t*>(scene);
    lit[0] = static_cast<float>(bytes[0xab6]) * (1.0f / 255.0f);
    lit[1] = static_cast<float>(bytes[0xab5]) * (1.0f / 255.0f);
    lit[2] = static_cast<float>(bytes[0xab4]) * (1.0f / 255.0f);
    direct[0] = static_cast<float>(bytes[0xab2]) * (1.0f / 255.0f);
    direct[1] = static_cast<float>(bytes[0xab1]) * (1.0f / 255.0f);
    direct[2] = static_cast<float>(bytes[0xab0]) * (1.0f / 255.0f);
    shadow[0] = static_cast<float>(bytes[0xaba]) * (1.0f / 255.0f);
    shadow[1] = static_cast<float>(bytes[0xab9]) * (1.0f / 255.0f);
    shadow[2] = static_cast<float>(bytes[0xab8]) * (1.0f / 255.0f);
    *strength = DNFloatBand(s_lightBand, 8, time) * *strength;
    sun[2] = DNField<float>(scene, 0xe8) * 192.0f + sun[2];        // DAT_142979a10
    *strength = (1.0f - DNField<float>(scene, 0xe8) * 0.75f) * *strength;
}

// Whether the clouds show: not under a sky override that hides them (the transition's first,
// then the three override skies).
// ref: FUN_14245f7b0
bool CloudsVisible(DNScene* scene, uint8_t* covered) {
    constexpr float SHOWN = 0.99f;                       // DAT_14263252c
    auto transition = DNField<uint8_t*>(scene, 0x80);
    if (transition == nullptr) {
        if (DNField<uint8_t>(scene, 0x79) != 0) {
            return true;
        }
    } else if (M2BaseModelIsLoadedAndReady(DNField<CM2BaseModel*>(transition, 0x38))
        && SHOWN < DNField<float>(transition, 0x4c)) {
        return DNField<uint8_t>(transition, 0x50) != 0;
    }
    for (int32_t i = 0; i < 3; i++) {
        auto sky = DNField<uint8_t*>(scene, 0xb8 + i * 8);
        if (sky != nullptr && M2BaseModelIsLoadedAndReady(DNField<CM2BaseModel*>(sky, 0x38))
            && SHOWN < DNField<float>(sky, 0x4c) && DNField<uint8_t>(sky, 0x50) == 0) {
            *covered = 1;
            return false;
        }
    }
    return true;
}

}  // namespace

// ref: FUN_142451e20
void DNCLOUDS::MarkDirty() {
    this->m_dirty = 1;
}

// ref: FUN_142452140
void DNCLOUDS::Generate(DNScene* scene) {
    float density = DNField<float>(scene, 0xadc);
    if (s_densityOverride != 0.0f) {
        density = s_densityOverride;
    }
    float clear = 1.0f - density;
    clear = clear < 0.0f ? 0.0f : std::min(clear, 1.0f);
    auto falloff = static_cast<uint8_t>(static_cast<int32_t>(clear * 255.0f));
    bool proceed;
    if (falloff != this->m_falloff && falloff == 0xff) {
        this->m_05 = 100;
        this->m_falloff = falloff;
        proceed = this->m_05 != 0;
    } else {
        this->m_falloff = falloff;
        proceed = falloff != 0xff || this->m_05 != 0;
    }
    if (!proceed) {
        return;
    }
    uint8_t covered = 0;
    if (this->m_enabled == 0 || !CloudsVisible(scene, &covered)) {
        this->m_dirty = 0;
        return;
    }
    this->m_18 = this->m_cells;
    if (this->m_dirty != 0) {
        this->m_cells = this->m_gridSize;
        this->m_1c = 0;
    }
    uint16_t z = this->m_0a;
    uint32_t octaves = static_cast<uint32_t>(this->m_34);
    uint32_t zHash0 = s_permutation[z >> 8];
    uint32_t zHash1 = s_permutation[static_cast<uint8_t>((z >> 8) + 1)];
    OCTAVE walk[5];
    const uint16_t* steps = &s_octaveSteps[this->m_quality * 5];
    for (uint32_t i = 0; i < octaves; i++) {
        uint16_t step = steps[i];
        uint16_t zInt = static_cast<uint16_t>(z >> 8);
        walk[i].m_yStep = step;
        walk[i].m_xStep = step;
        walk[i].m_zInt = zInt;
        walk[i].m_zInt2 = zInt;
        walk[i].m_y = static_cast<uint16_t>(static_cast<uint16_t>(this->m_1c) * step);
        walk[i].m_zNext = static_cast<uint16_t>(zInt + 1);
        walk[i].m_z = z;
        walk[i].m_12 = static_cast<uint16_t>(
            (static_cast<int32_t>(static_cast<uint32_t>(step) << (this->m_gridLog2 & 0x1f)) >> 8) + 2 + zInt);
        walk[i].m_amplitude = 1.0f / static_cast<float>(1 << i);
    }
    float lit[3] = {0.0f, 0.0f, 0.0f};
    float direct[3] = {0.0f, 0.0f, 0.0f};
    float shadow[3] = {0.0f, 0.0f, 0.0f};
    float sun[3] = {0.0f, 0.0f, 0.0f};
    float strength = 1.0f;
    CloudLighting(this, scene, lit, direct, shadow, sun, &strength);
    if (this->m_05 != 0) {
        this->m_05--;
    }
    float slopeX = 0.0f;
    float slopeY = 0.0f;
    for (uint32_t row = 0; row < static_cast<uint32_t>(this->m_cells); row++) {
        uint64_t offset = static_cast<uint64_t>(this->m_1c + row) << (this->m_gridLog2 & 0x3f);
        uint8_t* densities = this->m_density.m_data + offset;
        uint32_t* colors = this->m_colors.data() + offset;
        for (uint32_t i = 0; i < octaves; i++) {
            uint32_t y = walk[i].m_y >> 8;
            walk[i].m_yInt = static_cast<uint16_t>(y);
            walk[i].m_cachedX = 0xffffffff;
            walk[i].m_x = this->m_0a;
            uint32_t yNext = (y + 1) & 0xff;
            walk[i].m_yNext = static_cast<uint16_t>(yNext);
            walk[i].m_corners[0] = s_permutation[static_cast<uint8_t>(y + zHash0)];
            walk[i].m_corners[1] = s_permutation[static_cast<uint8_t>(yNext + zHash0)];
            walk[i].m_corners[2] = s_permutation[static_cast<uint8_t>(y + zHash1)];
            walk[i].m_corners[3] = s_permutation[static_cast<uint8_t>(yNext + zHash1)];
        }
        float rowF = static_cast<float>(static_cast<uint64_t>(static_cast<uint32_t>(this->m_1c + row)));
        float previous = 0.0f;
        for (uint32_t column = 0; column < static_cast<uint32_t>(this->m_gridSize); column++) {
            float sum = 0.0f;
            for (uint32_t i = 0; i < octaves; i++) {
                OCTAVE& o = walk[i];
                uint32_t x = o.m_x;
                uint32_t xInt = x >> 8;
                uint32_t xFrac = x & 0xff;
                o.m_xInt = xInt;
                if (xInt != o.m_cachedX) {
                    o.m_cachedX = xInt;
                    for (int32_t corner = 0; corner < 4; corner++) {
                        uint32_t at = o.m_corners[corner] + xInt;
                        float base = s_noise[s_permutation[at & 0xff]];
                        o.m_noise[corner * 2] = base;
                        o.m_noise[corner * 2 + 1] = s_noise[s_permutation[static_cast<uint8_t>(at + 1)]] - base;
                    }
                }
                float fx = s_cosine[xFrac];
                o.m_x = static_cast<uint16_t>(o.m_x + o.m_xStep);
                float a = fx * o.m_noise[1] + o.m_noise[0];
                float c = fx * o.m_noise[5] + o.m_noise[4];
                float b = fx * o.m_noise[3] + o.m_noise[2];
                float d = fx * o.m_noise[7] + o.m_noise[6];
                float fy = s_cosine[o.m_y & 0xff];
                float e1 = (b - a) * fy + a;
                float e2 = (d - c) * fy + c;
                float value = ((e2 - e1) * s_cosine[o.m_z & 0xff] + e1) * o.m_amplitude;
                sum += value;
                if (i == 2) {
                    float scale = static_cast<float>(1 << ((this->m_gridLog2 - 7) & 0x1f));
                    slopeX = previous - sum;
                    previous = sum;
                    float above = this->m_88.m_data[column];
                    this->m_88.m_data[column] = sum;
                    slopeX *= scale;
                    slopeY = (above - sum) * scale;
                }
            }
            if (densities[column] != 0) {
                this->m_05 = 100;
            }
            uint8_t cell = 0;
            uint32_t level = static_cast<uint32_t>(static_cast<int32_t>(std::nearbyint(sum * 64.0f + 128.0f))) & 0xff;  // DAT_14263a048, DAT_1426c707c
            if (0 <= static_cast<int32_t>(level - this->m_falloff)) {
                cell = s_falloff[level - this->m_falloff];
            }
            densities[column] = cell;
            if (cell == 0) {
                if (column != 0) {
                    colors[column] = colors[column - 1];
                    reinterpret_cast<uint8_t*>(&colors[column])[3] = 0;
                }
                continue;
            }
            float toSunX = sun[0] - static_cast<float>(column);
            float toSunY = sun[1] - rowF;
            auto shade = static_cast<uint8_t>((static_cast<uint8_t>(~cell) >> 1) + 0x40);
            float k = static_cast<float>(shade) * (1.0f / 255.0f);
            float r = lit[0] * k + shadow[0];
            float g = lit[1] * k + shadow[1];
            float bl = lit[2] * k + shadow[2];
            float facing = (slopeY * toSunY + slopeX * toSunX + sun[2])
                * (FastInverseSqrt(slopeY * slopeY + slopeX * slopeX + 1.0f)
                    * FastInverseSqrt(toSunY * toSunY + toSunX * toSunX + sun[2] * sun[2]));
            if (0.0f < facing) {
                facing *= strength;
                r = direct[0] * facing + r;
                g = direct[1] * facing + g;
                bl = direct[2] * facing + bl;
            }
            r = std::min(r, 1.0f);
            g = std::min(g, 1.0f);
            bl = std::min(bl, 1.0f);
            auto out = reinterpret_cast<uint8_t*>(&colors[column]);
            out[2] = static_cast<uint8_t>(static_cast<int32_t>(std::nearbyint(r * 255.0f)));
            out[1] = static_cast<uint8_t>(static_cast<int32_t>(std::nearbyint(g * 255.0f)));
            out[0] = static_cast<uint8_t>(static_cast<int32_t>(std::nearbyint(bl * 255.0f)));
            out[3] = densities[column];
        }
        for (uint32_t i = 0; i < octaves; i++) {
            walk[i].m_y = static_cast<uint16_t>(walk[i].m_y + walk[i].m_yStep);
        }
    }
    if (this->m_104 != GxDeviceBufferedFrames()) {
        for (void*& tex : this->m_textures) {
            if (tex != nullptr) {
                void* destroyed = tex;
                GxTexDestroy(&destroyed);
            }
        }
        this->m_textures.m_size = 0;
    }
    this->m_20 = static_cast<uint32_t>(this->m_cells);
    if (this->m_dirty != 0) {
        if (this->m_18 != -1) {
            this->m_cells = this->m_18;
        }
        this->m_dirty = 0;
    }
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_f_1424531e0 = (void*)&CloudGridPoint;
// ---- end of generated globals ----
