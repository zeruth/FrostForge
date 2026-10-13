// The loading screen's progress bar: three textures (frame, fill, border) in a strip at the
// bottom of the screen, the fill cut at the progress fraction. Drawn every frame while the
// loading screen shows it (FUN_14022ea60 from the client's frame), in the 4:3-ish area the
// loading screen's art covers.

#include <cstdint>

#include "client/LoadingBar.hpp"

#include "base/Coordinate.hpp"
#include "base/Matrix.hpp"
#include "client/GxWindow.hpp"
#include "gx/Gx.hpp"
#include "gx/GxBuf.hpp"
#include "gx/GxVertexFormat.hpp"
#include "gx/d3d12/CommandList.hpp"
#include "shadereffect/ShaderEffectPipeline.hpp"
#include "texture/Texture.hpp"

extern GxRenderContext* g_gxRenderContext;  // DAT_14348b658
bool GxDevExists();
uint16_t FontBlend(int32_t index);
uint32_t FontRaster(int32_t index);

// One quad of the bar (0x18): its texture's file, whether the progress cuts it, its centre and
// size as fractions of the screen.
struct LOADINGBARQUAD {
    int32_t m_fileDataId;               // 0x00
    uint8_t m_progress;                 // 0x04
    uint8_t m_pad05[3];
    float m_x;                          // 0x08
    float m_y;                          // 0x0C
    float m_width;                      // 0x10
    float m_height;                     // 0x14
};
static_assert(sizeof(LOADINGBARQUAD) == 0x18, "LOADINGBARQUAD is 0x18 bytes");

uint8_t s_loadingBarShown;              // DAT_142de0b35
int32_t s_loadingScreenAspect;          // DAT_142de0c04: 0 fills the screen, 1 and 2 by the window's shape, 3 at 9:8
HTEXTURE s_loadingBarTextures[3];       // DAT_142de0c20
float s_loadingBarProgress;             // DAT_142de0c48
CShaderEffect* s_loadingScreenEffect;   // DAT_142de0ca0: UILoadingScreen

namespace {

// DAT_14261a150
const LOADINGBARQUAD s_loadingBarQuads[3] = {
    { 0x18ba3a, 0, {}, 0.5f, 0.975f, 0.6f, 0.05f },
    { 0x202e4, 1, {}, 0.5f, 0.978f, 0.575f, 0.025f },
    { 0x18ba3b, 0, {}, 0.5f, 0.975f, 0.6f, 0.05f },
};

// The quads' texture coordinates (DAT_142cc8590) and their strip's indices (DAT_142cc85b0).
const float s_quadUvs[4][2] = { { 0.0f, 1.0f }, { 1.0f, 1.0f }, { 0.0f, 0.0f }, { 1.0f, 0.0f } };
const uint16_t s_quadIndices[4] = { 0, 1, 2, 3 };

// Each quad that has a texture, as a four-vertex strip (format 0xc: position, colour, UV).
// ref: FUN_140230490
void DrawQuads(GxRenderContext* context, uint64_t count, const LOADINGBARQUAD* quads, const HTEXTURE* textures) {
    for (uint64_t i = 0; i < count; i++) {
        if (textures[i] == nullptr) {
            continue;
        }
        const LOADINGBARQUAD& quad = quads[i];
        float halfWidth = quad.m_width * 0.5f;
        float halfHeight = quad.m_height * 0.5f;
        float left = quad.m_x - halfWidth;
        float right = quad.m_x + halfWidth;
        float top = halfHeight + quad.m_y;
        float bottom = quad.m_y - halfHeight;
        if (quad.m_progress != 0) {
            right = quad.m_width * s_loadingBarProgress + left;
        }
        float corners[4][3] = {
            { left, bottom, 0.0f }, { right, bottom, 0.0f }, { left, top, 0.0f }, { right, top, 0.0f },
        };
        void* tex = TextureGetGxTexOrDefault(textures[i]);
        LoadingScreenSetPipeline(context, 4, 1, 0);
        uint32_t stride = GxVertexFormatStride(0xc);
        GxBuf* vertices = GxBufTransient(context, 0, stride, 4);
        auto data = static_cast<uint8_t*>(context->BufLock(vertices));
        uint8_t* position = data + GxVertexFormatOffset(0xc, 0);
        uint8_t* color = data + GxVertexFormatOffset(0xc, 4);
        uint8_t* uv = data + GxVertexFormatOffset(0xc, 6);
        for (int32_t v = 0; v < 4; v++) {
            auto p = reinterpret_cast<float*>(position + v * stride);
            p[0] = corners[v][0];
            p[1] = corners[v][1];
            p[2] = corners[v][2];
            *reinterpret_cast<uint32_t*>(color + v * stride) = 0xffffffff;
            auto t = reinterpret_cast<float*>(uv + v * stride);
            t[0] = s_quadUvs[v][0];
            t[1] = s_quadUvs[v][1];
        }
        context->BufUnlock(vertices, stride * 4);
        GxSetVertexStream(context, vertices, 0xc);
        context->SetTexture(2, 0, static_cast<gxDx12Backend::Tex*>(tex));
        GxBuf* indices = GxBufTransient(context, 1, 2, 4);
        context->BufWrite(indices, s_quadIndices, 8, 0);
        context->SetIndexBuffer(indices, 1);
        context->DrawIndexed(4, 1, 0, 0, 0, 0);
    }
}

}  // namespace

// ref: FUN_140232110
void LoadingScreenSetPipeline(GxRenderContext* context, uint8_t topology, int32_t blend, int32_t raster) {
    uint16_t blendState = FontBlend(blend);
    uint32_t rasterState = FontRaster(raster);
    GxPipeline* pipeline = ShaderEffectGetPipeline(s_loadingScreenEffect, 0, 0, 0x700000020ull, blendState, rasterState,
        0xc, topology, 0xc, nullptr);
    context->SetPipeline(pipeline);
}

// ref: FUN_1402321b0
void LoadingScreenSetConstants(GxRenderContext* context, const float* projection) {
    float* constants = context->LockConstants(2, 0, 0x10);
    constants[0] = 0.0f;
    constants[1] = 0.0f;
    constants[2] = 0.0f;
    constants[3] = 0.0f;
    context->UnlockConstants(2, 0);
    float m[16];
    for (int32_t i = 0; i < 16; i++) {
        m[i] = projection[i];
    }
    GXSIZE size;
    WindowGetResolution(&size);
    constants = context->LockConstants(0, 0, 0x50);
    for (int32_t row = 0; row < 4; row++) {
        for (int32_t column = 0; column < 4; column++) {
            constants[row * 4 + column] = m[column * 4 + row];
        }
    }
    constants[0x10] = static_cast<float>(size.m_width);
    constants[0x11] = static_cast<float>(size.m_height);
    constants[0x12] = 0.0f;
    constants[0x13] = 1.0f;
    context->UnlockConstants(0, 0);
    constants = context->LockConstants(0, 1, 0x10);
    constants[0] = 0.0f;
    constants[1] = 0.0f;
    constants[2] = 0.0f;
    constants[3] = 0.0f;
    context->UnlockConstants(0, 1);
    // DAT_14261a9f0
    constants = context->LockConstants(0, 2, 0x10);
    constants[0] = 1.0f;
    constants[1] = 1.0f;
    constants[2] = 0.0f;
    constants[3] = 0.0f;
    context->UnlockConstants(0, 2);
}

// ref: FUN_140232c60
void LoadingScreenSetViewport(GxRenderContext* context, float minX, float maxX, float minY, float maxY, float minZ,
    float maxZ) {
    if (1.0f <= maxX) {
        maxX = 1.0f;
    }
    if (1.0f <= maxY) {
        maxY = 1.0f;
    }
    if (minX <= 0.0f) {
        minX = 0.0f;
    }
    if (minY <= 0.0f) {
        minY = 0.0f;
    }
    if (minX != context->Field<float>(0x1c) || maxX != context->Field<float>(0x20) || minY != context->Field<float>(0x24)
        || maxY != context->Field<float>(0x28) || minZ != context->Field<float>(0x2c) || maxZ != context->Field<float>(0x30)) {
        context->Field<float>(0x1c) = minX;
        context->Field<float>(0x20) = maxX;
        context->Field<float>(0x24) = minY;
        context->Field<float>(0x28) = maxY;
        context->Field<float>(0x2c) = minZ;
        context->Field<float>(0x30) = maxZ;
        context->Field<uint8_t>(0x18) = 1;
    }
}

// The bar is drawn in the loading screen's own area: the whole screen, or a centred band as
// wide (or tall) as the art's aspect allows. The viewport is restored afterwards.
// ref: FUN_14022ea60
void LoadingBarRender() {
    if (s_loadingBarShown == 0 || !GxDevExists()) {
        return;
    }
    GxRenderContext* context = g_gxRenderContext;
    float savedMinZ = context->Field<float>(0x2c);
    float savedMaxZ = context->Field<float>(0x30);
    float savedMinX = context->Field<float>(0x1c);
    float savedMaxX = context->Field<float>(0x20);
    float savedMinY = context->Field<float>(0x24);
    float savedMaxY = context->Field<float>(0x28);
    if (savedMinX != 0.0f || savedMaxX != 1.0f || savedMinY != 0.0f || savedMaxY != 1.0f || savedMinZ != 0.0f
        || savedMaxZ != 1.0f) {
        context->Field<uint8_t>(0x18) = 1;
        context->Field<float>(0x1c) = 0.0f;
        context->Field<float>(0x20) = 1.0f;
        context->Field<float>(0x24) = 0.0f;
        context->Field<float>(0x28) = 1.0f;
        context->Field<float>(0x2c) = 0.0f;
        context->Field<float>(0x30) = 1.0f;
    }
    float aspect = 1.0f;
    if (s_loadingScreenAspect != 0) {
        if (s_loadingScreenAspect == 1 || s_loadingScreenAspect == 2) {
            aspect = 1.2f;
            if (1.6f < g_coordinateSpace.m_aspect) {
                aspect = 1.3333334f;
                if (g_coordinateSpace.m_aspect < 1.7777778f) {
                    aspect = g_coordinateSpace.m_aspect * 0.75f;
                }
            }
        } else if (s_loadingScreenAspect == 3) {
            aspect = 1.125f;
        }
    }
    float ratio = g_coordinateSpace.m_aspectCompensation / aspect;
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX;
    float maxY = 1.0f;
    if (ratio <= 1.0f) {
        maxX = 1.0f;
        if (ratio < 1.0f) {
            minY = (1.0f - ratio) * 0.5f;
            maxY = ratio;
        }
    } else {
        minX = (1.0f - 1.0f / ratio) * 0.5f;
        maxX = 1.0f / ratio;
    }
    maxX = maxX + minX;
    if (minX <= 0.0f) {
        minX = 0.0f;
    }
    maxY = maxY + minY;
    if (minY <= 0.0f) {
        minY = 0.0f;
    }
    if (1.0f <= maxX) {
        maxX = 1.0f;
    }
    if (1.0f <= maxY) {
        maxY = 1.0f;
    }
    if (minX != context->Field<float>(0x1c) || maxX != context->Field<float>(0x20) || minY != context->Field<float>(0x24)
        || maxY != context->Field<float>(0x28) || context->Field<float>(0x2c) != 0.0f || context->Field<float>(0x30) != 1.0f) {
        context->Field<float>(0x1c) = minX;
        context->Field<float>(0x20) = maxX;
        context->Field<float>(0x24) = minY;
        context->Field<float>(0x28) = maxY;
        context->Field<float>(0x2c) = 0.0f;
        context->Field<float>(0x30) = 1.0f;
        context->Field<uint8_t>(0x18) = 1;
    }
    // DAT_14261a9d0: identity, then the unit square's projection
    float projection[16] = {
        1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
    };
    Orthographic(0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 500.0f, projection);
    LoadingScreenSetPipeline(context, 4, 0, 0);
    LoadingScreenSetConstants(context, projection);
    DrawQuads(context, 3, s_loadingBarQuads, s_loadingBarTextures);
    LoadingScreenSetViewport(context, savedMinX, savedMaxX, savedMinY, savedMaxY, savedMinZ, savedMaxZ);
}

// ---- GENERATED by tools/translate.py: the port globals translated code reaches ----
extern "C" void* const gen_p_142de0b35 = (void*)&s_loadingBarShown;
extern "C" void* const gen_p_142de0c20 = (void*)&s_loadingBarTextures;
extern "C" void* const gen_p_142de0ca0 = (void*)&s_loadingScreenEffect;
// ---- end of generated globals ----
