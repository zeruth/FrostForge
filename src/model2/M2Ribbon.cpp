#include "model2/M2Ribbon.hpp"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <xmmintrin.h>

#include "base/Matrix.hpp"
#include "base/RndSeed.hpp"
#include "db/DB2Tables.hpp"
#include "db/WowClientDB2.hpp"
#include "texture/Texture.hpp"

// Engine/Source/Model2 ribbons (0x141296530..0x141297630) over the GpuStripEmitter.

namespace {

// Ribbons are drawn at all (DAT_142cfbf74).
uint8_t s_ribbonsDrawn = 1;

// A ribbon's random stream's next value in 0..1 (FUN_141297630).
// ref: FUN_141297630
float RibbonRandom(CRndSeed* random) {
    return RndSeedUnit(random);
}

// The squared distance past which a new position is a jump and the strip starts again
// (DAT_1426c6510).
constexpr float RIBBON_JUMP = 10000.0f;

// Whether the newest edge is far enough or turned enough from the last to keep it, rather than
// put in place of the last (FUN_141297100). Edges here are six floats: top, then bottom.
// ref: FUN_141297100
bool RibbonNeedsEdge(const CRibbonEmitter* ribbon, float age, const float* edge, const float* last,
    const float* previous, const float* earlier) {
    bool keep = true;
    if (previous == nullptr || (1.1920929e-07f < ribbon->m_maxSampleTime && ribbon->m_maxSampleTime <= age)) {  // DAT_1426c6f7c
        return keep;
    }
    float x = edge[0] - edge[3];
    float y = edge[1] - edge[4];
    float z = edge[2] - edge[5];
    float length = y * y + x * x + z * z;
    if (2.3841858e-07f < length) {
        float scale = 1.0f / std::sqrt(length);
        x *= scale;
        y *= scale;
        z *= scale;
    }
    float px = previous[0] - previous[3];
    float py = previous[1] - previous[4];
    float pz = previous[2] - previous[5];
    float plength = py * py + px * px + pz * pz;
    if (2.3841858e-07f < plength) {
        float scale = 1.0f / std::sqrt(plength);
        px *= scale;
        py *= scale;
        pz *= scale;
    }
    keep = x * px + y * py + z * pz < ribbon->m_angleThreshold;
    if (keep || earlier == nullptr) {
        return keep;
    }
    float dx = edge[0] - last[0];
    float dy = edge[1] - last[1];
    float dz = edge[2] - last[2];
    float moved = dy * dy + dx * dx + dz * dz;
    const float minimum = 0.0099999998f;  // DAT_142cfbf78
    const float maximum = 100.0f;         // DAT_142cfbf7c
    if (maximum * maximum < moved) {
        return true;
    }
    if (moved <= minimum * minimum) {
        return keep;
    }
    float ex = previous[0] - earlier[0];
    float ey = previous[1] - earlier[1];
    float ez = previous[2] - earlier[2];
    float elength = ey * ey + ex * ex + ez * ez;
    if (std::fabs(elength) < 9.5367432e-07f) {  // DAT_14261a958
        return keep;
    }
    float n = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(elength)));
    ex *= n;
    ey *= n;
    ez *= n;
    float m = _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(moved)));
    keep = dy * m * ey + dx * m * ex + dz * m * ez < ribbon->m_angleThreshold;
    if (keep) {
        return keep;
    }
    float rx = previous[0] - edge[0];
    float ry = previous[1] - edge[1];
    float rz = previous[2] - edge[2];
    float along = ry * ey + rx * ex + rz * ez;
    float ox = rx - ex * along;
    float oy = ry - ey * along;
    float oz = rz - ez * along;
    return 2.0f * 2.0f < oy * oy + ox * ox + oz * oz;  // DAT_142cfbf80
}

// An edge's top and bottom points, packed.
void PackEdge(const GpuStripEdge* edge, float* out) {
    out[0] = edge->m_top[0];
    out[1] = edge->m_top[1];
    out[2] = edge->m_top[2];
    out[3] = edge->m_bottom[0];
    out[4] = edge->m_bottom[1];
    out[5] = edge->m_bottom[2];
}

// An edge laid down where the ribbon is: kept, or put in place of the last when the ribbon
// has not moved or turned enough; a jump starts the strip again (FUN_141296860).
// ref: FUN_141296860
void RibbonEmit(CRibbonEmitter* ribbon) {
    GpuStripEmitter* strip = &ribbon->m_strip;
    float above = ribbon->m_heightAbove;
    float edge[6];
    edge[2] = ribbon->m_direction.z * above + ribbon->m_position.z;
    edge[0] = ribbon->m_direction.x * above + ribbon->m_position.x;
    edge[1] = ribbon->m_direction.y * above + ribbon->m_position.y;
    float below = ribbon->m_heightBelow;
    edge[3] = ribbon->m_position.x - ribbon->m_direction.x * below;
    edge[4] = ribbon->m_position.y - ribbon->m_direction.y * below;
    edge[5] = ribbon->m_position.z - ribbon->m_direction.z * below;
    uint32_t live = static_cast<uint32_t>(strip->m_segments.m_head) - static_cast<uint32_t>(strip->m_segments.m_tail);
    float age = 0.0f;
    float last[6] = {};
    float previous[6] = {};
    float earlier[6] = {};
    const float* lastEdge = nullptr;
    const float* previousEdge = nullptr;
    const float* earlierEdge = nullptr;
    if (live != strip->m_dead) {
        uint32_t past = live - strip->m_dead;
        if (past != 1) {
            if (past != 2) {
                PackEdge(strip->m_edges.At(strip->m_edges.m_mask - 2 + strip->m_edges.m_head), earlier);
                earlierEdge = earlier;
            }
            PackEdge(strip->m_edges.At(strip->m_edges.m_mask - 1 + strip->m_edges.m_head), previous);
            previousEdge = previous;
            age = strip->m_currentLifetime
                - strip->m_segments.At(strip->m_segments.m_head - 1 + strip->m_segments.m_mask)->m_age;
        }
        PackEdge(strip->m_edges.At(strip->m_edges.m_head + strip->m_edges.m_mask), last);
        lastEdge = last;
        if ((ribbon->m_flags & 1) != 0) {
            float dy = edge[1] - last[1];
            float dx = edge[0] - last[0];
            float dz = edge[2] - last[2];
            if (RIBBON_JUMP < dy * dy + dx * dx + dz * dz) {
                strip->Reset();
                return;
            }
        }
    }
    if (!RibbonNeedsEdge(ribbon, age, edge, lastEdge, previousEdge, earlierEdge)) {
        strip->Pop();
    }
    strip->Push(&edge[0], &edge[3], nullptr);
}

}  // namespace

// ref: FUN_141296530
CRibbonEmitter* RibbonEmitterConstruct(void* memory) {
    auto ribbon = static_cast<CRibbonEmitter*>(memory);
    new (&ribbon->m_strip) GpuStripEmitter();
    ribbon->m_drawMatrix = nullptr;
    ribbon->m_drawView = nullptr;
    ribbon->m_100 = 0;
    ribbon->m_drawCamera = {0.0f, 0.0f, 0.0f};
    ribbon->m_color = 0xffffffff;
    ribbon->m_blend = 2;
    ribbon->m_uvSpeedU[0] = 0.0f;
    ribbon->m_textures[0] = nullptr;
    ribbon->m_uvOffset[0][0] = 0.0f;
    ribbon->m_uvOffset[0][1] = 0.0f;
    ribbon->m_uvScale[0][0] = 1.0f;
    ribbon->m_uvScale[0][1] = 1.0f;
    ribbon->m_textures[1] = nullptr;
    ribbon->m_uvScale[1][0] = 1.0f;
    ribbon->m_uvScale[1][1] = 1.0f;
    ribbon->m_uvOffset[1][0] = 0.0f;
    ribbon->m_uvOffset[1][1] = 0.0f;
    ribbon->m_textures[2] = nullptr;
    ribbon->m_uvScale[2][0] = 1.0f;
    ribbon->m_uvScale[2][1] = 1.0f;
    ribbon->m_uvOffset[2][0] = 0.0f;
    ribbon->m_uvOffset[2][1] = 0.0f;
    std::memset(ribbon->m_uvStart, 0, sizeof(ribbon->m_uvStart));
    ribbon->m_flags &= 0xf9;
    ribbon->m_blendMode = 0;
    ribbon->m_position = {0.0f, 0.0f, 0.0f};
    ribbon->m_direction = {0.0f, 0.0f, 0.0f};
    ribbon->m_colorOverride = {0.0f, 0.0f, 0.0f};
    ribbon->m_uvSpeedV[0] = 0.0f;
    ribbon->m_uvSpeedV[1] = 0.0f;
    ribbon->m_uvSpeedU[1] = 0.0f;
    ribbon->m_uvSpeedU[2] = 0.0f;
    ribbon->m_uvSpeedV[2] = 0.0f;
    return ribbon;
}

// ref: FUN_141296640
void RibbonEmitterDelete(CRibbonEmitter* ribbon) {
    ribbon->m_strip.Clear();
    for (uint32_t i = 0; i < 3; i++) {
        if (ribbon->m_textures[i] != nullptr) {
            TextureFree(&ribbon->m_textures[i]);
        }
    }
    ribbon->m_flags &= 0xf9;
    ribbon->m_strip.~GpuStripEmitter();
}

// ref: FUN_1412966b0
void RibbonEmitterSetParentMatrix(CRibbonEmitter* ribbon, const C44Matrix* matrix) {
    ribbon->m_strip.Transform(reinterpret_cast<const float*>(matrix));
}

// The textures and the blend, and the draw's texture types (FUN_141296d00).
// ref: FUN_141296d00
static void RibbonEmitterSetMaterial(CRibbonEmitter* ribbon, const uint32_t* blend, CTexture** textures,
    const uint32_t* types) {
    std::memcpy(ribbon->m_textureTypes, types, sizeof(ribbon->m_textureTypes));
    ribbon->m_blendMode = static_cast<int32_t>(*blend);
    for (uint32_t i = 0; i < 3; i++) {
        if (ribbon->m_textures[i] != nullptr) {
            TextureFree(&ribbon->m_textures[i]);
        }
    }
    for (uint32_t i = 0; i < 3; i++) {
        if (textures[i] != nullptr) {
            TextureAddRef(textures[i]);
            ribbon->m_textures[i] = textures[i];
        }
    }
}

// ref: FUN_1412966c0
void RibbonEmitterInitialize(CRibbonEmitter* ribbon, float lifetime, const uint32_t* color, CTexture** textures,
    const uint32_t* blend, const uint32_t* types) {
    ribbon->m_flags &= 0xfe;
    ribbon->m_quality = 0;
    RibbonEmitterSetMaterial(ribbon, blend, textures, types);
    ribbon->m_flags |= 8;
    ribbon->m_heightAbove = 10.0f;
    ribbon->m_heightBelow = 10.0f;
    ribbon->m_color = *color;
    GpuStripEmitter* strip = &ribbon->m_strip;
    strip->Reserve(0x40, false);
    strip->SetLifetime(lifetime);
    strip->SetEnabled(true);
    if (strip->m_detail == 0) {
        strip->SetDetail(1);
    }
    ribbon->m_flags |= 4;
    float u1;
    float v1;
    float u2;
    float v2;
    if (ribbon->m_textures[1] == nullptr) {
        ribbon->m_uvStart[0] = 0.0f;
        ribbon->m_uvStart[1] = 0.0f;
        u2 = 0.0f;
        v2 = 0.0f;
    } else {
        CRndSeed random;
        RndSeedInit(&random, 0);
        int32_t high = rand();  // FUN_142415c90
        uint16_t low = static_cast<uint16_t>(rand());
        RndSeedInit(&random, static_cast<uint32_t>(low) | static_cast<uint32_t>(high) << 16);
        v1 = RibbonRandom(&random);
        u1 = RibbonRandom(&random);
        ribbon->m_uvStart[0] = u1;
        ribbon->m_uvStart[1] = v1;
        v2 = RibbonRandom(&random);
        u2 = RibbonRandom(&random);
    }
    ribbon->m_uvStart[2] = u2;
    ribbon->m_uvStart[3] = v2;
    ribbon->m_uvOffset[1][0] = ribbon->m_uvStart[0];
    ribbon->m_uvOffset[1][1] = ribbon->m_uvStart[1];
    ribbon->m_uvOffset[2][0] = ribbon->m_uvStart[2];
    ribbon->m_uvOffset[2][1] = ribbon->m_uvStart[3];
}

// ref: FUN_141296840
bool RibbonEmitterIsIdle(const CRibbonEmitter* ribbon) {
    return (ribbon->m_flags & 4) != 0
        && static_cast<int32_t>(ribbon->m_strip.m_segments.m_head) - static_cast<int32_t>(ribbon->m_strip.m_segments.m_tail)
        == static_cast<int32_t>(ribbon->m_strip.m_dead);
}

// ref: FUN_141296af0
void RibbonEmitterDraw(CRibbonEmitter* ribbon, const RIBBONDRAWPARAMS* params, const C44Matrix* view,
    const C44Matrix* projection) {
    if (s_ribbonsDrawn == 0) {
        return;
    }
    float viewCopy[16];
    float projectionCopy[16];
    std::memcpy(viewCopy, view, sizeof(viewCopy));
    std::memcpy(projectionCopy, projection, sizeof(projectionCopy));
    ribbon->m_drawMatrix = params->m_matrix;
    ribbon->m_drawView = viewCopy;
    ribbon->m_drawProjection = projectionCopy;
    ribbon->m_drawCamera = *params->m_camera;
    ribbon->m_blend = static_cast<int32_t>(static_cast<int16_t>(ribbon->m_blendMode));
    ribbon->m_strip.Draw(params->m_context);
}

// ref: FUN_141296bc0
void RibbonEmitterSetAlpha(CRibbonEmitter* ribbon, float alpha) {
    reinterpret_cast<uint8_t*>(&ribbon->m_color)[3] = static_cast<uint8_t>(static_cast<int64_t>(alpha * 255.0f + 0.5f));
}

// ref: FUN_141296be0
void RibbonEmitterSetColor(CRibbonEmitter* ribbon, float r, float g, float b) {
    // The alpha byte taken back through 255 squared, which leaves it as it was.
    uint32_t alpha = static_cast<uint32_t>(static_cast<int64_t>(
        static_cast<float>(reinterpret_cast<const uint8_t*>(&ribbon->m_color)[3]) * 65025.0f + 0.5f)) & 0xff;  // DAT_142726888
    uint32_t red;
    uint32_t green;
    uint32_t blue;
    if ((ribbon->m_flags & 2) == 0) {
        red = static_cast<uint32_t>(static_cast<int64_t>(r * 255.0f + 0.5f));
        green = static_cast<uint32_t>(static_cast<int64_t>(g * 255.0f + 0.5f));
        blue = static_cast<uint32_t>(static_cast<int64_t>(b * 255.0f + 0.5f));
    } else {
        red = static_cast<uint32_t>(static_cast<int64_t>(ribbon->m_colorOverride.x * 255.0f + 0.5f));
        green = static_cast<uint32_t>(static_cast<int64_t>(ribbon->m_colorOverride.y * 255.0f + 0.5f));
        blue = static_cast<uint32_t>(static_cast<int64_t>(ribbon->m_colorOverride.z * 255.0f + 0.5f));
    }
    ribbon->m_color = ((alpha << 8 | (red & 0xff)) << 8 | (green & 0xff)) << 8 | (blue & 0xff);
}

// ref: FUN_141296cc0
void RibbonEmitterSetVisible(CRibbonEmitter* ribbon, bool visible) {
    ribbon->m_flags = static_cast<uint8_t>((visible ? 8 : 0) | (ribbon->m_flags & 0xf7));
}

// ref: FUN_141296ce0
static void RibbonEmitterSetLifetime(CRibbonEmitter* ribbon, float lifetime) {
    ribbon->m_strip.SetLifetime(lifetime);
}

// ref: FUN_141296cf0
void RibbonEmitterSetGravity(CRibbonEmitter* ribbon, float gravity) {
    ribbon->m_strip.SetGravity(gravity);
}

// ref: FUN_141296df0
void RibbonEmitterSetMatrix(CRibbonEmitter* ribbon, const float* matrix, const void* a3) {
    float up[4] = {matrix[4], matrix[5], matrix[6], matrix[7]};
    float at[4] = {matrix[12], matrix[13], matrix[14], matrix[15]};
    if (a3 != nullptr) {
        float inverse[16];
        MatrixRigidInverse(static_cast<const float*>(a3), inverse);
        float u[3];
        float p[3];
        for (uint32_t c = 0; c < 3; c++) {
            u[c] = up[2] * inverse[8 + c] + up[3] * inverse[12 + c] + up[1] * inverse[4 + c] + up[0] * inverse[c];
            p[c] = at[0] * inverse[c] + at[1] * inverse[4 + c] + at[2] * inverse[8 + c] + at[3] * inverse[12 + c];
        }
        std::memcpy(up, u, sizeof(u));
        std::memcpy(at, p, sizeof(p));
    }
    ribbon->m_flags |= 1;
    ribbon->m_position = {at[0], at[1], at[2]};
    ribbon->m_direction = {up[0], up[1], up[2]};
}

// ref: FUN_141296f00
void RibbonEmitterSetColorOverride(CRibbonEmitter* ribbon, float r, float g, float b) {
    ribbon->m_flags |= 2;
    ribbon->m_colorOverride = {r, g, b};
}

// ref: FUN_141296f20
void RibbonEmitterSetQuality(CRibbonEmitter* ribbon, uint32_t quality) {
    bool local;
    auto record = static_cast<const uint8_t*>(g_ribbonQualityDB.GetRecord(quality, false, &local));
    if (record == nullptr) {
        ribbon->m_quality = 0;
        return;
    }
    ribbon->m_quality = static_cast<uint16_t>(quality);
    (void)DB2FieldU32(record, 4, 0, &g_ribbonQualityDB);
    float texLodScale = g_ribbonQualityDB.FieldFloat(record, 3, 0);
    float maxSampleTime = g_ribbonQualityDB.FieldFloat(record, 1, 0);
    float angleThreshold = g_ribbonQualityDB.FieldFloat(record, 2, 0);
    uint8_t strips = DB2FieldU8(record, 0, 0, &g_ribbonQualityDB);
    ribbon->m_maxSampleTime = maxSampleTime;
    ribbon->m_angleThreshold = angleThreshold;
    if (4 < strips) {
        strips = 4;
    }
    ribbon->m_strip.SetDetail(strips);
    ribbon->m_strip.SetTexLodScale(texLodScale);
}

uint16_t RibbonEmitterGetQuality(const CRibbonEmitter* ribbon) {
    return ribbon->m_quality;
}

// ref: FUN_141297050
void RibbonEmitterSetTexture(CRibbonEmitter* ribbon, uint32_t layer, CTexture* texture) {
    if (texture != nullptr) {
        TextureAddRef(texture);
    }
    if (ribbon->m_textures[layer] != nullptr) {
        TextureFree(&ribbon->m_textures[layer]);
    }
    ribbon->m_textures[layer] = texture;
}

// ref: FUN_1412970b0
void RibbonEmitterSetTextureTransform(CRibbonEmitter* ribbon, uint32_t layer, const C4Quaternion* rotation,
    const C3Vector* scale, const C3Vector* translation) {
    (void)rotation;
    ribbon->m_uvScale[layer][0] = scale->x;
    ribbon->m_uvScale[layer][1] = scale->y;
    ribbon->m_uvSpeedU[layer] = translation->x;
    ribbon->m_uvSpeedV[layer] = translation->y;
}

// ref: FUN_1412970f0
void RibbonEmitterSetEnabled(CRibbonEmitter* ribbon, bool enabled) {
    ribbon->m_strip.SetEnabled(enabled);
}

// ref: FUN_141297490
void RibbonEmitterUpdate(CRibbonEmitter* ribbon, float delta, bool hidden) {
    if ((ribbon->m_flags & 9) == 9 && !hidden) {
        RibbonEmit(ribbon);
    }
    ribbon->m_strip.Update(delta);
    if (ribbon->m_uvSpeedU[0] == 0.0f) {
        ribbon->m_uvOffset[0][0] = 0.0f;
    } else {
        ribbon->m_uvOffset[0][0] = std::fmod(ribbon->m_uvSpeedU[0] * delta + ribbon->m_uvOffset[0][0], 1.0f);
    }
    if (ribbon->m_uvSpeedV[0] == 0.0f) {
        ribbon->m_uvOffset[0][1] = 0.0f;
    } else {
        ribbon->m_uvOffset[0][1] = std::fmod(ribbon->m_uvSpeedV[0] * delta + ribbon->m_uvOffset[0][1], 1.0f);
    }
    ribbon->m_uvOffset[1][0] = ribbon->m_uvSpeedU[1] == 0.0f
        ? ribbon->m_uvStart[0]
        : std::fmod(ribbon->m_uvSpeedU[1] * delta + ribbon->m_uvOffset[1][0], 1.0f);
    ribbon->m_uvOffset[1][1] = ribbon->m_uvSpeedV[1] == 0.0f
        ? ribbon->m_uvStart[1]
        : std::fmod(ribbon->m_uvSpeedV[1] * delta + ribbon->m_uvOffset[1][1], 1.0f);
    ribbon->m_uvOffset[2][0] = ribbon->m_uvSpeedU[2] == 0.0f
        ? ribbon->m_uvStart[2]
        : std::fmod(ribbon->m_uvSpeedU[2] * delta + ribbon->m_uvOffset[2][0], 1.0f);
    ribbon->m_uvOffset[2][1] = ribbon->m_uvSpeedV[2] == 0.0f
        ? ribbon->m_uvStart[3]
        : std::fmod(ribbon->m_uvSpeedV[2] * delta + ribbon->m_uvOffset[2][1], 1.0f);
}
