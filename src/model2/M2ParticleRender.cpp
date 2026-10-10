#include "model2/M2Particle.hpp"

#include <cmath>
#include <cstring>
#include <xmmintrin.h>

#include "base/Matrix.hpp"
#include "gx/Gx.hpp"
#include "gx/GxBuf.hpp"
#include "gx/GxRenderTargets.hpp"
#include "gx/GxVertexFormat.hpp"
#include "gx/d3d12/CommandList.hpp"
#include "shadereffect/ShaderEffectContext.hpp"
#include "shadereffect/ShaderEffectManager.hpp"
#include "texture/Texture.hpp"

// Engine/Source/ParticleSystem/ParticleSystem2.cpp, the draw: the tracks evaluated for a
// particle, the quads written into vertices in one of four formats, and the emitter's draw.

namespace {

constexpr float EPSILON = 2.3841858e-07f;      // DAT_14261a954
constexpr float FIXED16 = 3.0518509e-05f;      // DAT_1427228e4, 1 over 32767

// The quad's corners, as offsets (DAT_142cfbbd0) and texture coordinates (DAT_142cfbbf0).
const float s_cornerOffsets[4][2] = {{-1.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, 1.0f}, {1.0f, -1.0f}};
const float s_cornerUVs[4][2] = {{0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}};

// Bounds are taken around what the writers write (DAT_1430a1ee0, never set).
uint32_t s_particleBounds;

float RSqrt(float value) {
    return _mm_cvtss_f32(_mm_rsqrt_ss(_mm_set_ss(value)));
}

int32_t Round(float value) {
    return _mm_cvtss_si32(_mm_set_ss(value));
}

bool IsMultiTexture(const CParticleEmitter2* emitter) {
    return emitter->m_type == 2 || (emitter->m_type == 4 && emitter->m_textures[1] != nullptr);
}

bool HasCombiner(const CParticleEmitter2* emitter) {
    auto combiner = reinterpret_cast<const uint8_t*>(emitter->m_combiner);
    return combiner != nullptr && (combiner[0] != 0 || combiner[1] != 0);
}

// The particle's spin: its start angle and rate, drawn from its own seed when they vary
// (FUN_14124ba60).
// ref: FUN_14124ba60
void ParticleSpin(const CParticleEmitter2* emitter, const CParticle2* particle, float* base, float* rate) {
    if (emitter->m_baseSpinVary == 0.0f && emitter->m_spinVary == 0.0f) {
        *base = emitter->m_baseSpin;
        *rate = emitter->m_spin;
        return;
    }
    CRndSeed random;
    RndSeedInit(&random, particle->m_seed);
    if (emitter->m_baseSpinVary == 0.0f) {
        *base = emitter->m_baseSpin;
    } else {
        *base = emitter->m_baseSpinVary * RndSeedSigned(&random) + emitter->m_baseSpin;
    }
    if (emitter->m_spinVary == 0.0f) {
        *rate = emitter->m_spin;
    } else {
        *rate = emitter->m_spinVary * RndSeedSigned(&random) + emitter->m_spin;
    }
}

}  // namespace

// --- tracks ----------------------------------------------------------------------------------

// ref: FUN_14124b750
float ParticleTrackKey(const M2Array<uint16_t>* times, float t, uint32_t* first, uint32_t* second) {
    const int16_t* keys = reinterpret_cast<const int16_t*>(times->Data());
    uint16_t count = times->m_count;
    if (count != 2) {
        if (count != 3) {
            uint32_t low = 0;
            uint32_t index = 0;
            uint32_t last = static_cast<uint32_t>(count) - 1;
            if (last != 0) {
                uint32_t high = last;
                do {
                    index = (high + low) >> 1;
                    if (static_cast<float>(keys[index]) * FIXED16 <= t) {
                        low = index + 1;
                        if (last <= index + 1 || t < static_cast<float>(keys[low]) * FIXED16) {
                            break;
                        }
                    } else {
                        high = index - 1;
                    }
                    index = low;
                } while (index < high);
            }
            *first = index;
            *second = index + 1;
            int16_t a = keys[*first];
            return (32767.0f / static_cast<float>(static_cast<int16_t>(keys[index + 1] - a)))  // DAT_142724c3c
                * (t - static_cast<float>(a) * FIXED16);
        }
        float middle = static_cast<float>(keys[1]) * FIXED16;
        if (middle <= t) {
            *first = 1;
            *second = 2;
            return (t - middle) / (1.0f - middle);
        }
        t = t / middle;
    }
    *first = 0;
    *second = 1;
    return t;
}

// ref: FUN_14124c730
void ParticleTrackColor(const CParticleEmitter2* emitter, float t, uint8_t* out) {
    const M2PartTrack<C3Vector>* track = emitter->m_colorTrack;
    const float* values = reinterpret_cast<const float*>(track->m_values.Data());
    if (track->m_values.m_count == 1) {
        out[3] = 0xff;
        out[0] = static_cast<uint8_t>(Round(values[2]));
        out[1] = static_cast<uint8_t>(Round(values[1]));
        out[2] = static_cast<uint8_t>(Round(values[0]));
        return;
    }
    uint32_t first;
    uint32_t second;
    float f = ParticleTrackKey(&track->m_times, t, &first, &second);
    if ((emitter->m_flags & 0x10) != 0) {
        values = &emitter->m_colors[0][0];
    }
    const float* a = values + first * 3;
    const float* b = values + second * 3;
    float r = (b[0] - a[0]) * f + a[0];
    float g = (b[1] - a[1]) * f + a[1];
    float bl = (b[2] - a[2]) * f + a[2];
    out[3] = 0xff;
    out[0] = static_cast<uint8_t>(Round(bl));
    out[1] = static_cast<uint8_t>(Round(g));
    out[2] = static_cast<uint8_t>(Round(r));
}

// ref: FUN_14124c8a0
void ParticleTrackVector2(const M2PartTrack<C2Vector>* track, float t, float* out) {
    const float* values = reinterpret_cast<const float*>(track->m_values.Data());
    if (track->m_values.m_count == 1) {
        out[0] = values[0];
        out[1] = values[1];
        return;
    }
    uint32_t first;
    uint32_t second;
    float f = ParticleTrackKey(&track->m_times, t, &first, &second);
    float x = values[first * 2];
    float y = values[first * 2 + 1];
    out[0] = x + (values[second * 2] - x) * f;
    out[1] = y + (values[second * 2 + 1] - y) * f;
}

// ref: FUN_14124c950
float ParticleTrackFloat(const M2PartTrack<uint16_t>* track, float t) {
    const int16_t* values = reinterpret_cast<const int16_t*>(track->m_values.Data());
    if (track->m_values.m_count == 1) {
        return static_cast<float>(values[0]) * FIXED16;
    }
    uint32_t first;
    uint32_t second;
    float f = ParticleTrackKey(&track->m_times, t, &first, &second);
    float a = static_cast<float>(values[first]) * FIXED16;
    return (static_cast<float>(values[second]) * FIXED16 - a) * f + a;
}

// ref: FUN_14124c9f0
uint32_t ParticleTrackCell(const M2PartTrack<uint16_t>* track, float t) {
    const uint16_t* values = track->m_values.Data();
    if (track->m_values.m_count == 1) {
        return values[0];
    }
    uint32_t first;
    uint32_t second;
    float f = ParticleTrackKey(&track->m_times, t, &first, &second);
    uint32_t a = values[first];
    return static_cast<uint32_t>(
        Round(static_cast<float>(static_cast<int32_t>(values[second] - a)) * f + static_cast<float>(a)));
}

// --- a particle's state for drawing ----------------------------------------------------------

// The tracks at the particle's age: colour, alpha, size, cells, and the extended alpha cutoff
// (FUN_141247c60).
// ref: FUN_141247c60
static void ParticleEvaluate(const CParticleEmitter2* emitter, const CParticle2* particle, ParticleDrawState* out) {
    const CParticleGenerator* generator = emitter->m_generator;
    float life = (static_cast<float>(particle->m_lifeVariation) * FIXED16 * generator->m_lifespanVariation
                     + generator->m_lifespan)
        * generator->m_lifespanScale;
    if (life <= 0.001f) {
        life = 0.001f;
    }
    float t = particle->m_age / life;
    CRndSeed random;
    RndSeedInit(&random, particle->m_seed);
    float least = 1.0f <= t ? 1.0f : t;
    if (least <= 0.0f) {
        t = 0.0f;
    } else if (1.0f <= t) {
        t = 1.0f;
    }
    ParticleTrackColor(emitter, t, out->m_color);
    out->m_color[3] = static_cast<uint8_t>(Round(ParticleTrackFloat(emitter->m_alphaTrack, t) * 255.0f));
    ParticleTrackVector2(emitter->m_scaleTrack, t, out->m_scale);
    out->m_headCell = 0;
    out->m_tailCell = 0;
    if (emitter->m_headCellTrack->m_times.m_count == 0) {
        if ((emitter->m_flags & 0x100000) != 0) {
            uint32_t next = RndSeedNext(&random);
            out->m_headCell = static_cast<int32_t>(((static_cast<uint64_t>(emitter->m_cellMask) + 1) * next) >> 32);
        }
    } else {
        uint32_t cell = ParticleTrackCell(emitter->m_headCellTrack, t);
        out->m_headCell = static_cast<int32_t>((emitter->m_startCell + cell) & emitter->m_cellMask);
    }
    if (emitter->m_tailCellTrack->m_times.m_count != 0) {
        uint32_t cell = ParticleTrackCell(emitter->m_tailCellTrack, t);
        out->m_tailCell = static_cast<int32_t>((emitter->m_startCell + cell) & emitter->m_cellMask);
    }
    float scaleY = 1.0f;
    if ((emitter->m_flags & 0x800000) == 0) {
        float vary = RndSeedSigned(&random) * emitter->m_scaleVary.x + scaleY;
        if (vary <= 9.9999997e-05f) {  // DAT_1426c6f80
            vary = 9.9999997e-05f;
        }
        out->m_scale[0] = vary * out->m_scale[0];
        scaleY = vary;
    } else {
        float first = RndSeedSigned(&random);
        float second = RndSeedSigned(&random);
        float x = second * emitter->m_scaleVary.x + 1.0f;
        scaleY = emitter->m_scaleVary.y * first + 1.0f;
        if (x <= 9.9999997e-05f) {
            x = 9.9999997e-05f;
        }
        if (scaleY <= 9.9999997e-05f) {
            scaleY = 9.9999997e-05f;
        }
        out->m_scale[0] = x * out->m_scale[0];
    }
    out->m_scale[1] = scaleY * out->m_scale[1];
    float cutoff = 0.0f;
    const M2ExtendedParticle* extended = emitter->m_extended;
    if (extended != nullptr && extended->m_alphaCutoffTimes.m_count != 0) {
        cutoff = ParticleTrackFloat(reinterpret_cast<const M2PartTrack<uint16_t>*>(&extended->m_alphaCutoffTimes), t);
    }
    out->m_alphaCutoff = cutoff;
}

// The same from the fast path's two keys, in 16.16 fixed point (FUN_141248290).
// ref: FUN_141248290
static void ParticleEvaluateFast(const CParticleEmitter2* emitter, const CParticle2* particle, ParticleDrawState* out) {
    const float k = 65536.0f;  // DAT_1426d2d54
    int64_t alphaScale = static_cast<int32_t>(static_cast<int64_t>(emitter->m_alpha * k));
    int32_t life = static_cast<int32_t>(static_cast<int64_t>(emitter->m_generator->m_maxLifespan * k));
    int32_t least = 0x28f;  // DAT_142cfbb74
    if (least < life) {
        least = life;
    }
    int32_t age = static_cast<int32_t>(static_cast<int64_t>(particle->m_age * k));
    int32_t t = static_cast<int32_t>((static_cast<int64_t>(age) << 16) / static_cast<int64_t>(least));
    if (0x10000 <= t) {
        t = 0x10000;
    } else if (t < 1) {
        t = 0;
    }
    int32_t middle = static_cast<int32_t>(static_cast<int64_t>(emitter->m_midpoint * k));
    const CParticleKeySimple* key;
    int32_t f;
    if (t < middle) {
        key = emitter->m_fastKeys[0];
        f = static_cast<int32_t>((static_cast<int64_t>(t) << 16) / static_cast<int64_t>(middle));
    } else {
        key = emitter->m_fastKeys[1];
        f = static_cast<int32_t>((static_cast<int64_t>(t - middle) << 16) / static_cast<int64_t>(0x10000 - middle));  // DAT_142cfbb70
    }
    out->m_color[2] = static_cast<uint8_t>((f * key->m_colorDelta[0] + key->m_color[0]) >> 16);
    out->m_color[1] = static_cast<uint8_t>((key->m_colorDelta[1] * f + key->m_color[1]) >> 16);
    out->m_color[0] = static_cast<uint8_t>((key->m_colorDelta[2] * f + key->m_color[2]) >> 16);
    int64_t alpha = static_cast<int64_t>(f * key->m_alphaDelta + key->m_alpha) * alphaScale;
    out->m_color[3] = static_cast<uint8_t>(static_cast<int32_t>(alpha >> 16) >> 16);
    float scale = static_cast<float>(f) * 1.5258789e-05f * key->m_scaleDelta + key->m_scale;  // DAT_1426c6544
    out->m_scale[0] = scale;
    out->m_scale[1] = scale;
    out->m_headCell = static_cast<int32_t>(
        ((static_cast<uint32_t>((f * key->m_headCellDelta + key->m_headCell) >> 16) & 0xffff) + emitter->m_startCell)
        & emitter->m_cellMask);
    out->m_tailCell = static_cast<int32_t>(
        ((static_cast<uint32_t>((f * key->m_tailCellDelta + key->m_tailCell) >> 16) & 0xffff) + emitter->m_startCell)
        & emitter->m_cellMask);
    const M2ExtendedParticle* extended = emitter->m_extended;
    if (extended == nullptr || extended->m_alphaCutoffTimes.m_count == 0) {
        out->m_alphaCutoff = 0.0f;
    } else if (extended->m_alphaCutoffValues.m_count == 1) {
        out->m_alphaCutoff = static_cast<float>(static_cast<int16_t>(extended->m_alphaCutoffValues.Data()[0])) * FIXED16;
    } else {
        uint32_t first;
        uint32_t second;
        float frac = ParticleTrackKey(&extended->m_alphaCutoffTimes, static_cast<float>(t) * 1.5258789e-05f, &first, &second);
        const int16_t* values = reinterpret_cast<const int16_t*>(extended->m_alphaCutoffValues.Data());
        float a = static_cast<float>(values[first]) * FIXED16;
        out->m_alphaCutoff = (static_cast<float>(values[second]) * FIXED16 - a) * frac + a;
    }
}

// A particle readied: dropped when it twinkles out, else its tracks, its twinkle and the
// emitter's scales, its view position and its inverse size (FUN_141241370 from the tracks,
// FUN_141241550 from the fast keys).
// ref: FUN_141241370
// ref: FUN_141241550
static bool ParticlePrepare(const CParticleEmitter2* emitter, const CParticle2* particle, ParticleDrawState* out,
    const ParticleSpace* space, bool fast) {
    uint32_t twinkle = 0;
    if (emitter->m_twinklePercent < 1.0f || emitter->m_twinkleScale[1] != 0.0f) {
        twinkle = static_cast<uint32_t>(Round(emitter->m_twinkleSpeed * particle->m_age) + particle->m_seed) & 0x7f;
    }
    if (emitter->m_twinklePercent < g_particleTwinkle[twinkle]) {
        return false;
    }
    if (fast) {
        ParticleEvaluateFast(emitter, particle, out);
    } else {
        ParticleEvaluate(emitter, particle, out);
    }
    if (emitter->m_twinkleScale[1] != 0.0f) {
        float factor = emitter->m_twinkleScale[1] * g_particleTwinkle[twinkle] + emitter->m_twinkleScale[0];
        out->m_scale[1] = factor * out->m_scale[1];
        out->m_scale[0] = factor * out->m_scale[0];
    }
    if ((emitter->m_flags & 0x400) != 0) {
        out->m_scale[1] = emitter->m_parentScale * out->m_scale[1];
        out->m_scale[0] = emitter->m_parentScale * out->m_scale[0];
    }
    out->m_scale[0] = emitter->m_scale * out->m_scale[0];
    out->m_scale[1] = emitter->m_scale * out->m_scale[1];
    const float* m = space->m_matrix;
    float y = particle->m_position.y;
    float x = particle->m_position.x;
    float z = particle->m_position.z;
    out->m_position.x = y * m[4] + x * m[0] + z * m[8] + m[12];
    out->m_position.y = y * m[5] + x * m[1] + z * m[9] + m[13];
    out->m_position.z = y * m[6] + x * m[2] + z * m[10] + m[14];
    float inverse = 1.0f;
    if ((emitter->m_renderFlags & 8) != 0) {
        float size = std::sqrt(std::fabs(out->m_scale[1] * out->m_scale[0]));
        float clamped = 0.0099999998f;  // DAT_142636334
        if (0.0099999998f <= size) {
            clamped = size;
            if (2.5f <= size) {  // DAT_1426d2734
                clamped = 2.5f;
            }
        }
        inverse = 1.0f / clamped;
    }
    out->m_inverseSize = inverse;
    return true;
}

// --- the quads -------------------------------------------------------------------------------

namespace {

// A head quad's half axes in view space: screen-facing, turned by the spin or laid along the
// velocity, or in the world's orientation (the shared first half of the head writers).
void ParticleQuadAxes(CParticleEmitter2* emitter, const CParticle2* particle, ParticleDrawState* state,
    const ParticleSpace* space, float* r, float* u) {
    float base;
    float rate;
    ParticleSpin(emitter, particle, &base, &rate);
    uint32_t flags = emitter->m_flags;
    float sx = state->m_scale[0];
    float sy = state->m_scale[1];
    bool along = false;
    if ((flags >> 21 & 1) != 0) {
        const C3Vector& v = particle->m_velocity;
        if (EPSILON < v.x * v.x + v.y * v.y + v.z * v.z) {
            along = true;
        }
    }
    bool spinning = emitter->m_spin != 0.0f || emitter->m_spinVary != 0.0f;
    auto angle = [&]() {
        float a = rate * particle->m_age + base;
        if ((emitter->m_flags >> 16 & 1) != 0 && (particle->m_seed & 1) != 0) {
            a = -a;
        }
        return a;
    };
    if ((flags & 0x4000) == 0) {
        if (!along) {
            if (!spinning) {
                r[0] = sx;
                r[1] = 0.0f;
                r[2] = 0.0f;
                u[0] = 0.0f;
                u[1] = sy;
                u[2] = 0.0f;
                return;
            }
            float a = angle();
            float s = std::sin(a);  // FUN_14240ad20
            float c = std::cos(a);
            r[0] = sx * c;
            r[1] = sx * s;
            r[2] = 0.0f;
            u[0] = -(sy * s);
            u[1] = sy * c;
            u[2] = 0.0f;
            if ((flags >> 29 & 1) != 0) {
                state->m_position.x = u[0] + state->m_position.x;
                state->m_position.y = u[1] + state->m_position.y;
            }
            return;
        }
        const float* m = space->m_matrix;
        float dy = -particle->m_velocity.y;
        float dx = -particle->m_velocity.x;
        float dz = -particle->m_velocity.z;
        float tx = dy * m[4] + dx * m[0] + dz * m[8];
        float ty = dy * m[5] + dx * m[1] + dz * m[9];
        float tz = dy * m[6] + dx * m[2] + dz * m[10];
        float length = ty * ty + tx * tx + tz * tz;
        float n = length <= EPSILON ? 0.0f : RSqrt(length);
        r[0] = sx * tx * n;
        r[1] = sx * ty * n;
        r[2] = tz * n * sx;
        u[0] = sy * ty * n;
        u[1] = -(sy * tx * n);
        u[2] = 0.0f;
        return;
    }
    float m3[9];
    std::memcpy(m3, space->m_rotation, sizeof(m3));
    float width = sx;
    if (along) {
        float dx = -particle->m_velocity.x;
        float dy = -particle->m_velocity.y;
        float n = 0.0f;
        float planar = dy * dy + dx * dx;
        if (EPSILON < planar) {
            n = RSqrt(planar);
        }
        dy *= n;
        dx *= n;
        float turn[9] = {dx, dy, 0.0f, -dy, dx, 0.0f, 0.0f, 0.0f, 1.0f};  // DAT_14261aa10
        Matrix3Multiply(m3, turn, space->m_rotation);
        if (EPSILON < n) {
            const C3Vector& v = particle->m_velocity;
            width = width * (RSqrt(v.x * v.x + v.y * v.y + v.z * v.z) / n);
        }
    }
    if (emitter->m_type == 4) {
        float turned[9];
        Matrix3FromQuaternion(turned, reinterpret_cast<const CParticle2_Projected*>(particle)->m_rotation);
        Matrix3Multiply(m3, turned, space->m_rotation);
    }
    r[0] = m3[0] * width;
    r[1] = m3[1] * width;
    r[2] = m3[2] * width;
    u[0] = sy * m3[3];
    u[1] = sy * m3[4];
    u[2] = sy * m3[5];
    if (!spinning) {
        return;
    }
    float a = angle();
    if (emitter->m_type == 4 || (emitter->m_flags & 0x204000) == 0x204000) {
        float x = m3[6];
        float y = m3[7];
        float z = m3[8];
        float scale = 1.0f / std::sqrt(x * x + y * y + z * z);
        emitter->m_cameraAxis = {x * scale, y * scale, z * scale};
    }
    float spin[9];
    Matrix3FromAxisAngle(spin, a, &emitter->m_cameraAxis.x, true);
    float rr[3] = {r[0], r[1], r[2]};
    float uu[3] = {u[0], u[1], u[2]};
    for (uint32_t j = 0; j < 3; j++) {
        r[j] = rr[0] * spin[j] + rr[1] * spin[3 + j] + rr[2] * spin[6 + j];
        u[j] = uu[0] * spin[j] + uu[1] * spin[3 + j] + uu[2] * spin[6 + j];
    }
}

// A tail quad's four corners: from the particle back along its velocity for the tail's length,
// or a plain quad when that is too short on screen (the shared first half of the tail writers).
void ParticleTailCorners(const CParticleEmitter2* emitter, const CParticle2* particle, const ParticleDrawState* state,
    const ParticleSpace* space, float corners[4][3]) {
    float length = emitter->m_tailLength;
    if ((emitter->m_flags & 0x20000) != 0 && particle->m_age < length) {
        length = particle->m_age;
    }
    const float* m = space->m_matrix;
    float dy = -particle->m_velocity.y;
    float dx = -particle->m_velocity.x;
    float dz = -particle->m_velocity.z;
    float ty = (dy * m[5] + dx * m[1] + dz * m[9]) * length;
    float tx = (dy * m[4] + dx * m[0] + dz * m[8]) * length;
    float planar = ty * ty + tx * tx;
    const C3Vector& p = state->m_position;
    if (planar < 0.00077160494f) {  // DAT_142722ff0
        for (uint32_t i = 0; i < 4; i++) {
            corners[i][0] = s_cornerOffsets[i][0] * state->m_scale[0] + p.x;
            corners[i][1] = s_cornerOffsets[i][1] * state->m_scale[1] + p.y;
            corners[i][2] = p.z;
        }
        return;
    }
    float n = RSqrt(planar);
    float tz = (dy * m[6] + dx * m[2] + dz * m[10]) * length;
    float a = n * state->m_scale[0] * tx;
    float b = n * state->m_scale[1] * ty;
    corners[0][0] = p.x - b;
    corners[0][1] = a + p.y;
    corners[0][2] = p.z;
    corners[1][0] = b + p.x;
    corners[1][1] = p.y - a;
    corners[1][2] = p.z;
    corners[2][0] = (p.x + tx) - b;
    corners[2][1] = (p.y + ty) + a;
    corners[2][2] = p.z + tz;
    corners[3][0] = (p.x + tx) + b;
    corners[3][1] = (p.y + ty) - a;
    corners[3][2] = p.z + tz;
}

// A head quad's corners from its axes.
void ParticleHeadCorners(const ParticleDrawState* state, const float* r, const float* u, float corners[4][3]) {
    const C3Vector& p = state->m_position;
    corners[0][0] = (p.x - r[0]) + u[0];
    corners[0][1] = (p.y - r[1]) + u[1];
    corners[0][2] = (p.z - r[2]) + u[2];
    corners[1][0] = (p.x - r[0]) - u[0];
    corners[1][1] = (p.y - r[1]) - u[1];
    corners[1][2] = (p.z - r[2]) - u[2];
    corners[2][0] = r[0] + p.x + u[0];
    corners[2][1] = r[1] + p.y + u[1];
    corners[2][2] = r[2] + p.z + u[2];
    corners[3][0] = (r[0] + p.x) - u[0];
    corners[3][1] = (r[1] + p.y) - u[1];
    corners[3][2] = (r[2] + p.z) - u[2];
}

// The colour as the vertex stores it: r g b a.
float PackedColor(const ParticleDrawState* state) {
    uint32_t packed = static_cast<uint32_t>(state->m_color[2]) | static_cast<uint32_t>(state->m_color[1]) << 8
        | static_cast<uint32_t>(state->m_color[0]) << 16 | static_cast<uint32_t>(state->m_color[3]) << 24;
    float value;
    std::memcpy(&value, &packed, sizeof(value));
    return value;
}

// The four vertex layouts: position, colour, the cell's coordinates and the alpha cutoff and
// inverse size (0x20); the same with the two layers' coordinates (0x30); with the corner's own
// coordinates and the layers' offsets (0x38); and with the view's normal (0x2c).
enum ParticleFormat {
    PARTICLE_FORMAT_PLAIN,
    PARTICLE_FORMAT_LAYERS,
    PARTICLE_FORMAT_COMBINED,
    PARTICLE_FORMAT_LIT,
};

uint32_t FormatStride(ParticleFormat format) {
    switch (format) {
    case PARTICLE_FORMAT_PLAIN:
        return 0x20;
    case PARTICLE_FORMAT_LAYERS:
        return 0x30;
    case PARTICLE_FORMAT_COMBINED:
        return 0x38;
    default:
        return 0x2c;
    }
}

// One quad's four vertices.
void ParticleWriteQuad(const CParticleEmitter2* emitter, const CParticle2* particle, const ParticleDrawState* state,
    const ParticleSpace* space, ParticleFormat format, int32_t cell, float corners[4][3], float* out) {
    float column = static_cast<float>(static_cast<uint32_t>(emitter->m_columns - 1) & static_cast<uint32_t>(cell))
        * emitter->m_cellWidth;
    float row = static_cast<float>(cell >> (emitter->m_cellShift & 0x1f)) * emitter->m_cellHeight;
    float color = PackedColor(state);
    auto quad = reinterpret_cast<const CParticle2_Quad*>(particle);
    for (uint32_t i = 0; i < 4; i++) {
        float cu = s_cornerUVs[i][0];
        float cv = s_cornerUVs[i][1];
        *out++ = corners[i][0];
        *out++ = corners[i][1];
        *out++ = corners[i][2];
        if (format == PARTICLE_FORMAT_LIT) {
            *out++ = space->m_normal[0];
            *out++ = space->m_normal[1];
            *out++ = space->m_normal[2];
        }
        *out++ = color;
        *out++ = cu * emitter->m_cellWidth + column;
        *out++ = cv * emitter->m_cellHeight + row;
        if (format == PARTICLE_FORMAT_LAYERS) {
            *out++ = emitter->m_uvScale[0] * cu + quad->m_uv[0];
            *out++ = emitter->m_uvScale[0] * cv + quad->m_uv[1];
            *out++ = emitter->m_uvScale[1] * cu + quad->m_uv[2];
            *out++ = emitter->m_uvScale[1] * cv + quad->m_uv[3];
        } else if (format == PARTICLE_FORMAT_COMBINED) {
            *out++ = cu;
            *out++ = cv;
            *out++ = quad->m_uv[0];
            *out++ = quad->m_uv[1];
            *out++ = quad->m_uv[2];
            *out++ = quad->m_uv[3];
        }
        *out++ = state->m_alphaCutoff;
        *out++ = state->m_inverseSize;
    }
}

void ParticleWriteHead(CParticleEmitter2* emitter, uint8_t* vertices, const CParticle2* particle,
    ParticleDrawState* state, const ParticleSpace* space, ParticleFormat format) {
    float r[3];
    float u[3];
    ParticleQuadAxes(emitter, particle, state, space, r, u);
    float corners[4][3];
    ParticleHeadCorners(state, r, u, corners);
    ParticleWriteQuad(emitter, particle, state, space, format, state->m_headCell, corners,
        reinterpret_cast<float*>(vertices));
}

void ParticleWriteTail(CParticleEmitter2* emitter, uint8_t* vertices, const CParticle2* particle,
    ParticleDrawState* state, const ParticleSpace* space, ParticleFormat format) {
    float corners[4][3];
    ParticleTailCorners(emitter, particle, state, space, corners);
    ParticleWriteQuad(emitter, particle, state, space, format, state->m_tailCell, corners,
        reinterpret_cast<float*>(vertices));
}

}  // namespace

// The head and tail writers by format.
// ref: FUN_141241730
static void ParticleWriteHeadPlain(CParticleEmitter2* emitter, uint8_t* vertices, const CParticle2* particle,
    ParticleDrawState* state, const ParticleSpace* space) {
    ParticleWriteHead(emitter, vertices, particle, state, space, PARTICLE_FORMAT_PLAIN);
}

// ref: FUN_141242060
static void ParticleWriteHeadLayers(CParticleEmitter2* emitter, uint8_t* vertices, const CParticle2* particle,
    ParticleDrawState* state, const ParticleSpace* space) {
    ParticleWriteHead(emitter, vertices, particle, state, space, PARTICLE_FORMAT_LAYERS);
}

// ref: FUN_141242bf0
static void ParticleWriteHeadCombined(CParticleEmitter2* emitter, uint8_t* vertices, const CParticle2* particle,
    ParticleDrawState* state, const ParticleSpace* space) {
    ParticleWriteHead(emitter, vertices, particle, state, space, PARTICLE_FORMAT_COMBINED);
}

// ref: FUN_141243680
static void ParticleWriteHeadLit(CParticleEmitter2* emitter, uint8_t* vertices, const CParticle2* particle,
    ParticleDrawState* state, const ParticleSpace* space) {
    ParticleWriteHead(emitter, vertices, particle, state, space, PARTICLE_FORMAT_LIT);
}

// ref: FUN_141244030
static void ParticleWriteTailPlain(CParticleEmitter2* emitter, uint8_t* vertices, const CParticle2* particle,
    ParticleDrawState* state, const ParticleSpace* space) {
    ParticleWriteTail(emitter, vertices, particle, state, space, PARTICLE_FORMAT_PLAIN);
}

// ref: FUN_141244600
static void ParticleWriteTailLayers(CParticleEmitter2* emitter, uint8_t* vertices, const CParticle2* particle,
    ParticleDrawState* state, const ParticleSpace* space) {
    ParticleWriteTail(emitter, vertices, particle, state, space, PARTICLE_FORMAT_LAYERS);
}

// ref: FUN_141244fc0
static void ParticleWriteTailCombined(CParticleEmitter2* emitter, uint8_t* vertices, const CParticle2* particle,
    ParticleDrawState* state, const ParticleSpace* space) {
    ParticleWriteTail(emitter, vertices, particle, state, space, PARTICLE_FORMAT_COMBINED);
}

// ref: FUN_141245570
static void ParticleWriteTailLit(CParticleEmitter2* emitter, uint8_t* vertices, const CParticle2* particle,
    ParticleDrawState* state, const ParticleSpace* space) {
    ParticleWriteTail(emitter, vertices, particle, state, space, PARTICLE_FORMAT_LIT);
}

using ParticleWriter = void (*)(CParticleEmitter2* emitter, uint8_t* vertices, const CParticle2* particle,
    ParticleDrawState* state, const ParticleSpace* space);

// The emitter's live particles written, head then tail quads, counting those drawn, and the
// bounds grown around the vertices when bounds are kept (the four loops differ only in their
// writers and stride).
static void ParticleWriteLoop(CParticleEmitter2* emitter, uint8_t* vertices, uint32_t count, const float* bounds,
    const ParticleSpace* space, ParticleWriter head, ParticleWriter tail, uint32_t stride) {
    int32_t type = emitter->m_type;
    uint8_t* cursor = vertices;
    uint32_t written = 0;
    if (type >= 0 && type <= 4) {
        bool fast = (emitter->m_flags & 0x1000000) != 0;
        emitter->m_drawnCount = 0;
        for (uint32_t i = 0; i < count; i++) {
            auto particle = reinterpret_cast<const CParticle2*>(emitter->m_list.Get(i));
            ParticleDrawState state;
            if (!ParticlePrepare(emitter, particle, &state, space, fast)) {
                continue;
            }
            if ((emitter->m_flags & 4) != 0) {
                head(emitter, cursor, particle, &state, space);
                cursor += stride * 4;
            }
            if ((emitter->m_flags & 8) != 0) {
                tail(emitter, cursor, particle, &state, space);
                cursor += stride * 4;
            }
            emitter->m_drawnCount++;
        }
        written = static_cast<uint32_t>((cursor - vertices) / stride);
    }
    if (s_particleBounds == 0 || written == 0) {
        return;
    }
    float* box = emitter->m_bounds;
    for (uint32_t i = 0; i < written; i++) {
        auto v = reinterpret_cast<const float*>(vertices + i * stride);
        float x = v[1] * bounds[4] + v[0] * bounds[0] + v[2] * bounds[8] + bounds[12];
        float y = v[1] * bounds[5] + v[0] * bounds[1] + v[2] * bounds[9] + bounds[13];
        float z = v[1] * bounds[6] + v[0] * bounds[2] + v[2] * bounds[10] + bounds[14];
        if (x < box[0]) {
            box[0] = x;
        }
        if (y < box[1]) {
            box[1] = y;
        }
        if (z < box[2]) {
            box[2] = z;
        }
        if (box[3] < x) {
            box[3] = x;
        }
        if (box[4] < y) {
            box[4] = y;
        }
        if (box[5] < z) {
            box[5] = z;
        }
    }
}

// ref: FUN_141245bf0
static void ParticleWritePlain(CParticleEmitter2* emitter, uint8_t* vertices, uint32_t count, const float* bounds,
    const ParticleSpace* space) {
    ParticleWriteLoop(emitter, vertices, count, bounds, space, ParticleWriteHeadPlain, ParticleWriteTailPlain, 0x20);
}

// ref: FUN_1412463a0
static void ParticleWriteLayers(CParticleEmitter2* emitter, uint8_t* vertices, uint32_t count, const float* bounds,
    const ParticleSpace* space) {
    ParticleWriteLoop(emitter, vertices, count, bounds, space, ParticleWriteHeadLayers, ParticleWriteTailLayers, 0x30);
}

// ref: FUN_141246be0
static void ParticleWriteCombined(CParticleEmitter2* emitter, uint8_t* vertices, uint32_t count, const float* bounds,
    const ParticleSpace* space) {
    ParticleWriteLoop(emitter, vertices, count, bounds, space, ParticleWriteHeadCombined, ParticleWriteTailCombined,
        0x38);
}

// ref: FUN_141247420
static void ParticleWriteLit(CParticleEmitter2* emitter, uint8_t* vertices, uint32_t count, const float* bounds,
    const ParticleSpace* space) {
    ParticleWriteLoop(emitter, vertices, count, bounds, space, ParticleWriteHeadLit, ParticleWriteTailLit, 0x2c);
}

// The loop the emitter's kind and format take (inlined at both of its callers).
void ParticleEmitterWrite(CParticleEmitter2* emitter, uint8_t* vertices, uint32_t count, const float* bounds,
    ParticleSpace* space) {
    int32_t type = emitter->m_type;
    if (IsMultiTexture(emitter)) {
        if (HasCombiner(emitter)) {
            ParticleWriteCombined(emitter, vertices, count, bounds, space);
            return;
        }
        ParticleWriteLayers(emitter, vertices, count, bounds, space);
        return;
    }
    if (type == 3) {
        ParticleWriteLayers(emitter, vertices, count, bounds, space);
        return;
    }
    if ((emitter->m_renderFlags & 1) == 0) {
        ParticleWritePlain(emitter, vertices, count, bounds, space);
        return;
    }
    ParticleWriteLit(emitter, vertices, count, bounds, space);
}

// The writers' matrices: the particles' space to the view, the view's z axis, and for world
// oriented quads the 3x3 and the camera axis (FUN_14124dc00).
// ref: FUN_14124dc00
void ParticleEmitterSetSpace(CParticleEmitter2* emitter, const float* attachment, const float* view, ParticleSpace* space) {
    if ((emitter->m_flags & 0x200) == 0) {
        if (attachment == nullptr) {
            std::memcpy(space->m_matrix, view, 16 * sizeof(float));
        } else {
            MatrixMultiply(space->m_matrix, attachment, view);
        }
    } else {
        MatrixMultiply(space->m_matrix, emitter->m_parentMatrix, view);
    }
    space->m_normal[0] = view[8];
    space->m_normal[1] = view[9];
    space->m_normal[2] = view[10];
    if ((emitter->m_flags & 0x4000) == 0) {
        return;
    }
    Matrix3FromMatrix(space->m_rotation, space->m_matrix);
    if ((emitter->m_flags & 0x200) != 0 && EPSILON <= std::fabs(emitter->m_parentScale)) {
        Matrix3Scale(space->m_rotation, 1.0f / emitter->m_parentScale);
    }
    float x = space->m_rotation[6];
    float y = space->m_rotation[7];
    float z = space->m_rotation[8];
    float length = x * x + y * y + z * z;
    if (length <= EPSILON) {
        emitter->m_cameraAxis = {0.0f, 0.0f, 1.0f};
        return;
    }
    float scale = 1.0f / std::sqrt(length);
    emitter->m_cameraAxis = {x * scale, y * scale, z * scale};
}

// --- the draw --------------------------------------------------------------------------------

// The quads' indices: 0 1 2 3 2 1 for each four vertices, through the whole buffer
// (FUN_14124d100).
// ref: FUN_14124d100
static void ParticleFillIndices(GxRenderContext* context, GxBuf* buf) {
    auto data = static_cast<uint16_t*>(context->BufLock(buf));
    uint16_t base = 0;
    for (uint16_t* cursor = data; cursor < data + 0x1fff8; cursor += 6) {
        cursor[0] = base;
        cursor[1] = static_cast<uint16_t>(base + 1);
        cursor[2] = static_cast<uint16_t>(base + 2);
        cursor[3] = static_cast<uint16_t>(base + 3);
        cursor[4] = static_cast<uint16_t>(base + 2);
        cursor[5] = static_cast<uint16_t>(base + 1);
        base = static_cast<uint16_t>(base + 4);
    }
    context->BufUnlock(buf, 0);
}

// The indices made with the device, and the six effects looked up (FUN_14124aa50).
// ref: FUN_14124aa50
void ParticleSystemOnDeviceCreated(void* param) {
    (void)param;
    GxBuf* buf = GxBufCreateSized(1, 0, 0x3fff0, "CParticleEmitter2_idx");
    g_particleIndices = buf;
    if (GxDevExists() && (buf->m_flags & 0x3000000) != 0x3000000) {
        ParticleFillIndices(g_gxRenderContext, g_particleIndices);
    }
    g_particleEffects[0] = ShaderEffectFind("Particle_Unlit_T1");
    g_particleEffects[1] = ShaderEffectFind("Particle_Lit_T1");
    g_particleEffects[2] = ShaderEffectFind("Particle_Unlit_2ColorTex_3AlphaTex");
    g_particleEffects[3] = ShaderEffectFind("Particle_Unlit_3ColorTex_3AlphaTex");
    g_particleEffects[4] = ShaderEffectFind("Particle_Unlit_3ColorTex_3AlphaTex_UV");
    g_particleEffects[5] = ShaderEffectFind("Particle_Refraction");
}

// LAB_14124ac20
void ParticleSystemOnDeviceDestroyed(void* param) {
    (void)param;
    GxBufDestroy(&g_particleIndices);
}

// ref: FUN_14124f4c0
int32_t ParticleEmitterCompare(const CParticleEmitter2* left, const CParticleEmitter2* right) {
    if (right->m_blend <= left->m_blend) {
        if (left->m_blend != right->m_blend) {
            return 1;
        }
        if (right->m_renderFlags <= left->m_renderFlags) {
            if (left->m_renderFlags != right->m_renderFlags) {
                return 1;
            }
            if (right->m_type <= left->m_type) {
                if (right->m_type < left->m_type) {
                    return 1;
                }
                for (uint32_t i = 0; i < 3; i++) {
                    auto a = reinterpret_cast<uintptr_t>(left->m_textures[i]);
                    auto b = reinterpret_cast<uintptr_t>(right->m_textures[i]);
                    if (a < b) {
                        return -1;
                    }
                    if (a != b) {
                        return 1;
                    }
                }
                return 0;
            }
        }
    }
    return -1;
}

// ref: FUN_14124bce0
uint8_t ParticleEmitterVertexFormat(const CParticleEmitter2* emitter) {
    int32_t type = emitter->m_type;
    if (type == 2 || (type == 4 && emitter->m_textures[1] != nullptr)) {
        return HasCombiner(emitter) ? 0x10 : 0xf;
    }
    if (type == 3) {
        return 0xf;
    }
    return static_cast<uint8_t>((~emitter->m_renderFlags & 1) << 2 | 9);
}

// ref: FUN_14124cc90
bool ParticleEmitterTexturesReady(const CParticleEmitter2* emitter) {
    if (g_particleDensityEnabled == 0) {
        return false;
    }
    uint32_t count = IsMultiTexture(emitter) ? 3 : 1;
    for (uint32_t i = 0; i < count; i++) {
        // The reference asks for the first texture every time.
        if (TextureGetGxTex(emitter->m_textures[0]) == nullptr) {
            return false;
        }
    }
    return true;
}

// ref: FUN_14124ca70
bool ParticleEmitterBatchable(const CParticleEmitter2* emitter, const CParticleEmitter2* other) {
    if (emitter->m_blend != other->m_blend || emitter->m_renderFlags != other->m_renderFlags
        || emitter->m_type != other->m_type || emitter->m_vertexFormat != other->m_vertexFormat) {
        return false;
    }
    bool multi = IsMultiTexture(emitter);
    uint32_t count = multi ? 3 : 1;
    for (uint32_t i = 0; i < count; i++) {
        if (emitter->m_textures[i] != other->m_textures[i]) {
            return false;
        }
    }
    bool combined = multi && HasCombiner(emitter);
    bool theirs = IsMultiTexture(other) && HasCombiner(other);
    if (combined != theirs) {
        return false;
    }
    const M2ExtendedParticle* a = emitter->m_extended;
    const M2ExtendedParticle* b = other->m_extended;
    if (a != nullptr && b != nullptr
        && (4.7683716e-07f <= std::fabs(a->m_colorMult - b->m_colorMult)  // DAT_14263241c
            || 4.7683716e-07f <= std::fabs(a->m_alphaMult - b->m_alphaMult))) {
        return false;
    }
    if (combined) {
        auto mine = reinterpret_cast<const uint8_t*>(emitter->m_combiner);
        auto yours = reinterpret_cast<const uint8_t*>(other->m_combiner);
        if (mine[0] != yours[0] || mine[1] != yours[1]
            || 4.7683716e-07f <= std::fabs(emitter->m_uvScale[0] - other->m_uvScale[0])
            || 4.7683716e-07f <= std::fabs(emitter->m_uvScale[1] - other->m_uvScale[1])) {
            return false;
        }
    }
    return ((other->m_flags >> 15) & 1) == ((emitter->m_flags >> 15) & 1);
}

// ref: FUN_14124f2c0
void ParticleEmitterSetConstants(CParticleEmitter2* emitter, GxRenderContext* context, ShaderEffectContext* shader,
    const float* environment) {
    context->SetTexture(2, 0, static_cast<gxDx12Backend::Tex*>(TextureGetGxTexOrDefaultRecreate(emitter->m_textures[0])));
    if (IsMultiTexture(emitter)) {
        for (uint32_t slot = 1; slot < 3; slot++) {
            context->SetTexture(2, slot,
                static_cast<gxDx12Backend::Tex*>(TextureGetGxTexOrDefaultRecreate(emitter->m_textures[slot])));
        }
    }
    float first[4];
    first[0] = (emitter->m_renderFlags & 0x10) == 0 ? 2.0f : 4.0f;
    first[1] = (emitter->m_flags & 0x8000) == 0 ? 1.0f / environment[0] : 0.0f;
    first[2] = shader->Get160();
    first[3] = emitter->m_viewFade;
    shader->SetBatchVectors(first, 0, 1);
    float second[4];
    second[0] = emitter->m_extended == nullptr ? 1.0f : emitter->m_extended->m_colorMult;
    second[1] = 1.0f;
    second[2] = 1.0f;
    second[3] = 0.0f;
    shader->SetBatchVectors(second, 1, 1);
}

// ref: FUN_14124f140
void ParticleEmitterSetTextureMatrix(const CParticleEmitter2* emitter, ShaderEffectContext* shader) {
    shader->ResetTextureMatrix(0);
    if (IsMultiTexture(emitter) && HasCombiner(emitter)) {
        auto combiner = reinterpret_cast<const uint8_t*>(emitter->m_combiner);
        float split[4] = {static_cast<float>(combiner[0]), static_cast<float>(combiner[1]), 0.0f, 0.0f};
        shader->SetBatch724(split);
        float scale[4] = {emitter->m_uvScale[0], emitter->m_uvScale[1], 0.0f, 0.0f};
        shader->SetLayerTransform(scale, 0, 1);
    }
}

// ref: FUN_14124f440
void ParticleEmitterBindOutline(const CParticleEmitter2* emitter, GxRenderContext* context, const uint8_t* outline) {
    if (emitter->m_type == 3 && outline != nullptr && (emitter->m_flags & 0x44000) != 0x44000) {
        context->BindTexture(2, 10, *reinterpret_cast<gxDx12Backend::Tex* const*>(outline + 0x48), 0);
        context->SetSampler(2, 10, 0x800200000000ull);
    }
}

// ref: FUN_14124d790
void ParticleEmitterDrawBatch(const CParticleEmitter2* emitter, GxRenderContext* context, GxBuf* vertices,
    uint32_t vertexCount, uint32_t indexCount, uint8_t shadingRate) {
    (void)vertexCount;
    GxBuf* indices = g_particleIndices;
    if (g_particleDensityEnabled == 0) {
        return;
    }
    if (GxDevExists() && (indices->m_flags & 0x3000000) != 0x3000000) {
        ParticleFillIndices(context, g_particleIndices);
    }
    GxSetVertexStream(context, vertices, ParticleEmitterVertexFormat(emitter));
    context->SetIndexBuffer(g_particleIndices, 1);
    context->DrawIndexed(indexCount, 1, 0, 0, 0, shadingRate);
}

// The emitter's particles drawn: copied from what the update prebuilt or written now into
// `vertices`, or into a transient buffer and drawn on their own when `vertices` is null
// (FUN_14124d0a0, its body FUN_14124d860).
// ref: FUN_14124d0a0
// ref: FUN_14124d860
void ParticleEmitterRender(CParticleEmitter2* emitter, GxRenderContext* context, const C44Matrix* view,
    const C44Matrix* matrix, uint8_t* vertices, M2ParticlePool* pool) {
    emitter->m_bounds[0] = 3.4028235e+38f;
    emitter->m_bounds[1] = 3.4028235e+38f;
    emitter->m_bounds[3] = -3.4028235e+38f;
    emitter->m_bounds[4] = -3.4028235e+38f;
    emitter->m_bounds[5] = -3.4028235e+38f;
    emitter->m_bounds[2] = 3.4028235e+38f;
    if (emitter->m_list.m_live.m_size == 0) {
        emitter->m_drawnCount = 0;
        return;
    }
    if (emitter->m_type == 1) {
        return;
    }
    if (!ParticleEmitterTexturesReady(emitter)) {
        emitter->m_drawnCount = 0;
        return;
    }
    auto v = reinterpret_cast<const float*>(view);
    float inverse[16];
    MatrixRigidInverse(v, inverse);
    uint32_t perParticle = emitter->m_verticesPerParticle;
    uint32_t count = 0x4000 / perParticle;
    if (emitter->m_list.m_live.m_size <= count) {
        count = static_cast<uint32_t>(emitter->m_list.m_live.m_size);
    }
    GxBuf* buf = nullptr;
    if (vertices == nullptr) {
        uint32_t stride = GxVertexFormatStride(ParticleEmitterVertexFormat(emitter));
        buf = GxBufTransient(context, 0, stride, perParticle * count);
        vertices = static_cast<uint8_t*>(context->BufLock(buf));
    }
    if (g_particlePrebuild != 0) {
        if (-1 < emitter->m_prebuiltSize && -1 < emitter->m_prebuiltOffset) {
            std::memcpy(vertices, pool->m_data + emitter->m_prebuiltOffset, static_cast<size_t>(emitter->m_prebuiltSize));
        }
    } else {
        float m[16];
        std::memcpy(m, v, sizeof(m));
        float move[3] = {-emitter->m_origin.x, -emitter->m_origin.y, -emitter->m_origin.z};
        MatrixTranslate(m, move);
        float space4[16];
        float normal[3] = {0.0f, 0.0f, 1.0f};
        float rotation[9];
        ParticleSpace space = {space4, normal, rotation};
        ParticleEmitterSetSpace(emitter, reinterpret_cast<const float*>(matrix), m, &space);
        ParticleEmitterWrite(emitter, vertices, count, inverse, &space);
    }
    if (buf == nullptr) {
        return;
    }
    context->BufUnlock(buf, 0);
    uint32_t drawn = emitter->m_drawnCount;
    uint32_t indices = emitter->m_indicesPerParticle;
    if (drawn == 0 || g_particleDensityEnabled == 0) {
        return;
    }
    if (GxDevExists() && (g_particleIndices->m_flags & 0x3000000) != 0x3000000) {
        ParticleFillIndices(context, g_particleIndices);
    }
    GxSetVertexStream(context, buf, ParticleEmitterVertexFormat(emitter));
    context->SetIndexBuffer(g_particleIndices, 1);
    context->DrawIndexed(drawn * indices, 1, 0, 0, 0, 0);
}
