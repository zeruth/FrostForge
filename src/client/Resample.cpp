#include "client/Resample.hpp"
#include "client/FrameBufferCache.hpp"
#include "client/RenderSettings.hpp"
#include "gx/Gx.hpp"
#include "gx/GxPipeline.hpp"
#include "gx/GxRenderTargets.hpp"
#include "gx/d3d12/DeviceD3d12.hpp"
#include "gx/d3d12/Objects.hpp"
#include "os/Os.hpp"
#include "shadereffect/ShaderEffectContext.hpp"
#include "util/Unimplemented.hpp"
#include <cmath>
#include <cstring>

namespace {

// DAT_1430a5ae0: the resample pipelines by (target kind * 2 + blend) * 2 + variant, then the
// FSR ones (DAT_1430a5b20) by the same index.
const uint32_t PIPELINE_COUNT = 16;
GxPipeline* s_pipelines[PIPELINE_COUNT];

// DAT_142726bb8: the two destination kinds (the window's, and the 8-bit one).
const uint32_t s_targetKinds[2] = {0xc, 5};
// DAT_142cfc2e0: the first target's blend, replacing or blending.
const uint16_t s_blends[2] = {0xf000, 0xf00d};
// DAT_142cfc2e8: the sampler a copy reads with (its flags word set when the pipelines are made).
uint64_t s_copySampler = 0x0000800200000001ull;

const uint32_t RESAMPLE_VERTEX_SHADER = 0x87;
const uint32_t RESAMPLE_PIXEL_SHADER = 0xd3;
const uint32_t FSR_PIXEL_SHADER = 0x7a;

// Takes every pipeline of `source` into `dest`, releasing the ones `dest` held.
// ref: FUN_1412a1f60
GxPipeline** PipelinesTake(GxPipeline** dest, GxPipeline** source) {
    for (uint32_t i = 0; i < PIPELINE_COUNT; i++) {
        GxPipeline* pipeline = source[i];
        source[i] = nullptr;
        GxPipeline* previous = dest[i];
        dest[i] = pipeline;
        if (previous != nullptr) {
            GxPipelineDestroy(&previous);
        }
    }
    return dest;
}

// ref: FUN_1412a1de0
void PipelinesDestroy(GxPipeline** pipelines) {
    for (uint32_t i = PIPELINE_COUNT; i-- > 0;) {
        GxPipeline* pipeline = pipelines[i];
        if (pipeline != nullptr) {
            GxPipelineDestroy(&pipeline);
        }
    }
}

// A borrowed target's size and description (FUN_1412a0420).
// ref: FUN_1412a0420
void TempSet(FRAMEBUFFERTEMP* temp, const char* name, const int32_t* size, const FRAMEBUFFERTARGET* target) {
    (void)name;
    temp->m_width = size[0];
    temp->m_height = size[1];
    temp->m_target = *target;
}

// A float as IEEE half, rounded to nearest (FUN_1423643b0).
uint16_t FloatToHalf(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    uint32_t sign = (bits >> 16) & 0x8000;
    int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xff) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffff;
    if (((bits >> 23) & 0xff) == 0xff) {
        return static_cast<uint16_t>(sign | 0x7c00 | (mantissa != 0 ? 0x200 : 0));
    }
    if (exponent >= 0x1f) {
        return static_cast<uint16_t>(sign | 0x7c00);
    }
    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<uint16_t>(sign);
        }
        mantissa |= 0x800000;
        uint32_t shift = static_cast<uint32_t>(14 - exponent);
        uint32_t half = mantissa >> shift;
        if (((mantissa >> (shift - 1)) & 1) != 0) {
            half++;
        }
        return static_cast<uint16_t>(sign | half);
    }
    uint32_t half = sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13);
    if ((mantissa & 0x1000) != 0) {
        half++;
    }
    return static_cast<uint16_t>(half);
}

// The FSR passes: EASU to upscale `source` into `dest` (its FsrEasuCon constants: the input
// to output scale and offset, then the input texel steps), RCAS to sharpen it.
// ref: FUN_1412a2380
void ResampleEasu(GxRenderContext* context, const float* rect, gxDx12Backend::Tex* source, gxDx12Backend::Tex* dest,
    uint8_t blend) {
    GXRENDERTARGETS targets;
    std::memset(&targets, 0, sizeof(targets));
    GxRenderTargetsAttach(&targets, 1, dest);
    GxRenderTargetsCount(&targets);
    context->Clear(&targets, static_cast<uint16_t>(blend ^ 1), 0, 1.0f, 0, 0);
    float outWidth = static_cast<float>(dest->m_width);
    float outHeight = static_cast<float>(dest->m_height);
    float inWidth = static_cast<float>(source->m_width);
    float inHeight = static_cast<float>(source->m_height);
    float* c = context->LockConstants(2, 0, 0x50);
    c[0] = (1.0f / outWidth) * inWidth;
    c[1] = (1.0f / outHeight) * inHeight;
    c[2] = inWidth * 0.5f * (1.0f / outWidth) - 0.5f;   // DAT_14261a988
    c[3] = inHeight * 0.5f * (1.0f / outHeight) - 0.5f;
    c[4] = 1.0f / inWidth;
    c[5] = 1.0f / inHeight;
    c[6] = 1.0f / inWidth;
    c[7] = -(1.0f / inHeight);
    c[8] = -(1.0f / inWidth);
    c[9] = (1.0f / inHeight) + (1.0f / inHeight);
    c[10] = 1.0f / inWidth;
    c[11] = (1.0f / inHeight) + (1.0f / inHeight);
    c[12] = 0.0f;
    c[13] = (1.0f / inHeight) * 4.0f;                    // DAT_1426c975c
    for (int32_t i = 14; i < 20; i++) {
        c[i] = 0.0f;
    }
    context->UnlockConstants(2, 0);
    uint32_t kind = dest->m_format == 2 ? 2 : 0;
    context->SetPipeline(s_pipelines[8 + (kind + blend) * 2]);
    context->BindTexture(2, 1, source, 0);
    context->SetSampler(2, 0, s_copySampler);
    float* vertex = context->LockConstants(0, 1, 0x10);
    std::memcpy(vertex, rect, 0x10);
    context->UnlockConstants(0, 1);
    context->Draw(3, 1, 0, 0, 0);
}

// RCAS over `source` into `dest`: FsrRcasCon's exp2(-sharpness), as a float and as two halves.
// ref: FUN_1412a21c0
void ResampleRcas(GxRenderContext* context, const float* rect, gxDx12Backend::Tex* source, gxDx12Backend::Tex* dest,
    float sharpness) {
    GXRENDERTARGETS targets;
    std::memset(&targets, 0, sizeof(targets));
    GxRenderTargetsAttach(&targets, 1, dest);
    GxRenderTargetsCount(&targets);
    context->Clear(&targets, 1, 0, 1.0f, 0, 0);
    float* c = context->LockConstants(2, 0, 0x50);
    float scale = exp2f(-sharpness);                     // FUN_142416098
    uint16_t half = FloatToHalf(scale);
    uint32_t packed = static_cast<uint32_t>(half) | (static_cast<uint32_t>(half) << 16);
    std::memset(c, 0, 0x50);
    c[0] = scale;
    std::memcpy(&c[1], &packed, 4);
    context->UnlockConstants(2, 0);
    context->SetPipeline(s_pipelines[9 + (dest->m_format == 2 ? 4 : 0)]);
    float* vertex = context->LockConstants(0, 1, 0x10);
    std::memcpy(vertex, rect, 0x10);
    context->UnlockConstants(0, 1);
    context->BindTexture(2, 1, source, 0);
    context->SetSampler(2, 0, s_copySampler);
    context->Draw(3, 1, 0, 0, 0);
}

// One full-screen triangle from `source` into `dest`: cleared first unless blending, the
// pipeline by the destination's format, the blend and the mode, the rectangle as the vertex
// constants.
// ref: FUN_1412a2cb0
void ResampleBlit(GxRenderContext* context, const float* rect, gxDx12Backend::Tex* source, gxDx12Backend::Tex* dest,
    uint8_t mode, uint8_t blend) {
    GXRENDERTARGETS targets;
    std::memset(&targets, 0, sizeof(targets));
    GxRenderTargetsAttach(&targets, 1, dest);
    GxRenderTargetsCount(&targets);
    context->Clear(&targets, static_cast<uint16_t>(blend ^ 1), 0, 1.0f, 0, 0);
    uint32_t kind = dest->m_format == 2 ? 2 : 0;
    context->SetPipeline(s_pipelines[(kind + blend) * 2 + (mode == 2 ? 1 : 0)]);
    float* constants = context->LockConstants(0, 1, 0x10);
    std::memcpy(constants, rect, 0x10);
    context->UnlockConstants(0, 1);
    context->BindTexture(2, 0, source, 0);
    uint64_t sampler = s_copySampler;
    if (mode != 0) {
        uint32_t usage = source->m_usage;
        uint16_t flags = static_cast<uint16_t>(((usage >> 3) & 1) | 0x8002);
        flags = static_cast<uint16_t>((flags & ~0x3e) | ((usage >> 0xd) << 1 & 0x3e));
        uint8_t bytes[8] = {
            static_cast<uint8_t>(usage & 7),
            static_cast<uint8_t>((usage >> 4) & 7),
            static_cast<uint8_t>((usage >> 7) & 7),
            static_cast<uint8_t>((usage >> 10) & 7),
            static_cast<uint8_t>(flags),
            static_cast<uint8_t>(flags >> 8),
            0,
            0,
        };
        std::memcpy(&sampler, bytes, 8);
    }
    context->SetSampler(2, 0, sampler);
    context->Draw(3, 1, 0, 0, 0);
}

}  // namespace

// Per destination kind, per blend: the copy and bilinear variants, and when the device can run
// them the FSR passes (EASU, RCAS; their 16-bit variants when not on D3D11 Windows 10).
// ref: FUN_1412a2650
void ResampleDeviceCreated(void* param) {
    (void)param;
    uint16_t fsrVariant = 0;
    if (GxApiIsD3d11() || OsGetVersion()->m_major < 10) {
        fsrVariant = 1;
    }
    s_copySampler = (s_copySampler & 0xffff000000000000ull)
        | (static_cast<uint64_t>((static_cast<uint16_t>(s_copySampler >> 32) & 0xffc2) | 2) << 32);
    GXPIPELINEKEY key;
    std::memset(&key, 0, sizeof(key));
    key.m_topology = 3;
    key.m_raster[2] = 1;
    key.m_depthStencil[0] = 0x20;
    key.m_depthStencil[4] = 7;
    key.m_vertexShader = RESAMPLE_VERTEX_SHADER;
    for (uint32_t kind = 0; kind < 2; kind++) {
        RENDERTARGETFORMATS formats = RenderTargetFormats(s_targetKinds[kind]);
        std::memcpy(key.m_targetFormats, &formats, 4);
        key.m_depthFormat = formats.m_depth;
        for (uint32_t blend = 0; blend < 2; blend++) {
            key.m_pixelShader = RESAMPLE_PIXEL_SHADER;
            key.m_blend = s_blends[blend];
            for (uint16_t variant = 0; variant < 2; variant++) {
                key.m_psPermute = variant;
                GxPipeline* pipeline = GxPipelineCreate(&key, "Resample");
                uint32_t index = (blend + kind * 2) * 2 + variant;
                GxPipeline* previous = s_pipelines[index];
                s_pipelines[index] = pipeline;
                if (previous != nullptr) {
                    GxPipelineDestroy(&previous);
                }
            }
            if (*reinterpret_cast<uint32_t*>(GxDeviceCaps() + 8) != 0) {
                key.m_pixelShader = FSR_PIXEL_SHADER;
                for (uint16_t pass = 0; pass < 2; pass++) {
                    key.m_psPermute = static_cast<uint16_t>(pass * 2 + fsrVariant);
                    GxPipeline* pipeline = GxPipelineCreate(&key, pass == 0 ? "FSR_EASU" : "FSR_RCAS");
                    uint32_t index = 8 + (blend + kind * 2) * 2 + pass;
                    GxPipeline* previous = s_pipelines[index];
                    s_pipelines[index] = pipeline;
                    if (previous != nullptr) {
                        GxPipelineDestroy(&previous);
                    }
                }
            }
        }
    }
}

// ref: FUN_1412a2960
void ResampleDeviceDestroyed(void* param) {
    (void)param;
    GxPipeline* none[PIPELINE_COUNT] = {};
    PipelinesTake(s_pipelines, none);
    PipelinesDestroy(none);
}

// ref: FUN_1412a28d0
void ResampleShutdown() {
    GxRemoveDeviceCreatedCallback(ResampleDeviceCreated, nullptr);
    GxRemoveDeviceDestroyedCallback(ResampleDeviceDestroyed, nullptr);
    GxPipeline* none[PIPELINE_COUNT] = {};
    PipelinesTake(s_pipelines, none);
    PipelinesDestroy(none);
}

// The mode falls back to a copy at the same size and to bilinear when shrinking; sharpening
// goes through a borrowed target the size of `dest` unless blending.
// ref: FUN_1412a29c0
void ResampleToTarget(GxRenderContext* context, const void* environment, gxDx12Backend::Tex* source,
    gxDx12Backend::Tex* dest, const RESAMPLEOPTIONS* options, bool blend) {
    context->UnbindTextures();
    float rect[4];
    ShaderEffectEnvironmentRect(static_cast<const uint8_t*>(environment), rect);
    uint8_t mode = options->m_mode;
    bool sharpen = options->m_sharpen != 0 && !blend;
    int32_t destSize[2] = {dest->m_width, dest->m_height};
    bool sameSize = source->m_width == dest->m_width && source->m_height == dest->m_height;
    bool shrinking = dest->m_width < source->m_width || dest->m_height < source->m_height;
    uint8_t blitMode = sameSize ? 0 : mode;
    if (shrinking && (blitMode == 0 || blitMode == 3)) {
        blitMode = 2;
    }
    FRAMEBUFFERTEMP temp;
    FrameBufferTempClear(&temp);
    gxDx12Backend::Tex* target = dest;
    if (!sameSize || sharpen) {
        if (!sameSize) {
            if (sharpen) {
                FRAMEBUFFERTARGET desc = {};
                desc.m_format = dest->m_format;
                desc.m_renderTarget = 1;
                desc.m_shaderResource = 1;
                TempSet(&temp, mode == 3 ? "EASU Output UAV" : "Resample Output", destSize, &desc);
                FrameBufferTempAcquire(&temp);
                target = static_cast<gxDx12Backend::Tex*>(temp.m_tex);
            }
        }
        if (!sameSize || !sharpen) {
            if (mode == 3) {
                ResampleEasu(context, rect, source, target, blend ? 1 : 0);
            } else {
                ResampleBlit(context, rect, source, target, blitMode, blend ? 1 : 0);
            }
            source = target;
        }
        if (sharpen) {
            ResampleRcas(context, rect, source, dest, options->m_sharpness);
        }
    } else if (mode == 3) {
        // the same size, no sharpening: EASU still, as the reference does
        ResampleEasu(context, rect, source, target, blend ? 1 : 0);
    } else {
        ResampleBlit(context, rect, source, target, blitMode, blend ? 1 : 0);
    }
    if (temp.m_tex != nullptr) {
        FrameBufferTempRelease(&temp);
    }
}

// The upscaler and sharpness the settings ask for: a render scale above 1 copies or upscales
// bilinearly at most; sharpening needs a sharpness and either the always-sharpen flag or FSR at
// a reduced scale.
// ref: FUN_14129d910
RESAMPLEOPTIONS* RenderSettingsResampleOptions(RESAMPLEOPTIONS* out) {
    uint8_t mode = g_renderSettings.m_upscaler;
    float scale = g_renderSettings.m_renderScale;
    float sharpness = g_renderSettings.m_sharpness;
    out->m_mode = mode;
    out->m_sharpness = sharpness;
    if (1.0f < scale && (mode == 0 || mode == 3)) {
        out->m_mode = 2;
        mode = 2;
    }
    if (0.0f <= sharpness && ((g_renderSettings.m_flags & 0x400) != 0 || (mode == 3 && scale < 1.0f))) {
        out->m_sharpen = 1;
        return out;
    }
    out->m_sharpen = 0;
    return out;
}
