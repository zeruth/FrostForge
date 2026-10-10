#include "model2/GpuStripEmitter.hpp"

#include <cmath>
#include <cstring>

#include "model2/M2Shared.hpp"
#include "storm/Memory.hpp"
#include "storm/String.hpp"
#include "util/Unimplemented.hpp"

// Engine/Source/Services/GpuStripEmitter.cpp (0x141298340..0x14129d3e0).

namespace {

const char s_stripFile[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Services\\GpuStripEmitter.cpp";
const char s_stripHeader[] = "D:\\BuildServer\\A\\work-git\\wow\\Engine\\Source\\Services\\GpuStripEmitter.h";

constexpr float FLT_LARGE = 3.4028235e+38f;

// A ring's array grown or shrunk to `count`, new elements zeroed (FUN_14129d240 for the edges,
// FUN_14129d0a0 for the segments): half again its capacity when it grows, the storage 16-byte
// aligned.
template <class T>
void RingResize(GpuStripRing<T>* ring, uint64_t count) {
    uint64_t size = ring->m_size;
    if (count < size) {
        ring->m_size = count;
        return;
    }
    if (size < count) {
        uint64_t field = ring->m_capacity;
        if ((field & 0x7fffffffffffffffull) < count) {
            uint64_t grown = (field >> 1 & 0x3fffffffffffffffull) + (field & 0x7fffffffffffffffull);
            if (grown < count) {
                grown = count;
            }
            ring->m_capacity = ((field ^ grown) & 0x8000000000000000ull) ^ grown;
            auto data = static_cast<T*>(M2AlignedAlloc(static_cast<uint32_t>(grown * sizeof(T)), s_stripHeader, 0x59, 0x12, 0));
            for (uint64_t i = 0; i < ring->m_size; i++) {
                data[i] = ring->m_data[i];
            }
            if (static_cast<int64_t>(ring->m_capacity) >= 0) {
                M2FreeFileData(ring->m_data);
            }
            ring->m_capacity &= 0x7fffffffffffffffull;
            ring->m_data = data;
        }
        for (uint64_t i = ring->m_size; i < count; i++) {
            std::memset(&ring->m_data[i], 0, sizeof(T));
        }
        ring->m_size = count;
    }
}

// The capacity fitted to the size, when the ring owns more than it holds (FUN_14129cf50 and
// FUN_14129ce90 move the elements).
template <class T>
void RingShrink(GpuStripRing<T>* ring) {
    uint64_t size = ring->m_size;
    if (size < (ring->m_capacity & 0x7fffffffffffffffull) && static_cast<int64_t>(ring->m_capacity) >= 0) {
        if (size == 0) {
            M2FreeFileData(ring->m_data);
            ring->m_data = nullptr;
            ring->m_capacity &= 0x8000000000000000ull;
            return;
        }
        auto data = static_cast<T*>(M2AlignedAlloc(static_cast<uint32_t>(size * sizeof(T)), s_stripHeader, 0x59, 0x12, 0));
        for (uint64_t i = 0; i < size; i++) {
            data[i] = ring->m_data[i];
        }
        M2FreeFileData(ring->m_data);
        ring->m_data = data;
        ring->m_capacity = size & 0x7fffffffffffffffull;
    }
}

// The segment ring holding `count`, its array the next power of two (FUN_14129cfb0).
void SegmentsSetCapacity(GpuStripRing<GpuStripSegment>* ring, uint64_t count) {
    uint64_t rounded = count;
    if ((count & (count - 1)) != 0) {
        int32_t bit = 63;
        while ((count >> bit) == 0) {
            bit--;
        }
        rounded = 1ull << ((bit + 1) & 0x3f);
    }
    RingResize(ring, rounded);
    RingShrink(ring);
    ring->m_max = count;
    ring->m_mask = rounded - 1;
}

void BoxGrow(float* box, const float* p) {
    if (p[0] < box[0]) {
        box[0] = p[0];
    }
    if (p[1] < box[1]) {
        box[1] = p[1];
    }
    if (p[2] < box[2]) {
        box[2] = p[2];
    }
    if (box[3] < p[0]) {
        box[3] = p[0];
    }
    if (box[4] < p[1]) {
        box[4] = p[1];
    }
    if (box[5] < p[2]) {
        box[5] = p[2];
    }
}

// A point through a matrix, its w taken as 1.
void TransformPoint(float* p, const float* m) {
    float x = p[0];
    float y = p[1];
    float z = p[2];
    p[0] = y * m[4] + x * m[0] + z * m[8] + m[12];
    p[1] = y * m[5] + x * m[1] + z * m[9] + m[13];
    p[2] = y * m[6] + x * m[2] + z * m[10] + m[14];
    p[3] = y * m[7] + x * m[3] + z * m[11] + m[15];
}

}  // namespace

GpuStripEmitter::GpuStripEmitter() {
    this->m_78 = -1;
    this->m_edges = {};
    this->m_segments.m_data = nullptr;
    this->m_segments.m_size = 0;
    this->m_segments.m_capacity = 0;
    this->m_segments.m_max = 0;
    this->m_segments.m_mask = 0;
    this->m_segments.m_head = 0;
    this->m_segments.m_tail = 0;
    this->m_extra = nullptr;
    this->m_lifetime = 0.0f;
    this->m_lastAge = 0.0f;
    this->m_gravity = 0.0f;
    this->m_texLodScale = 1.0f;
    this->m_detail = 1;
    for (uint32_t i = 0; i < 3; i++) {
        this->m_bounds[i] = FLT_LARGE;
        this->m_bounds[3 + i] = -FLT_LARGE;
        this->m_drawBounds[i] = FLT_LARGE;
        this->m_drawBounds[3 + i] = -FLT_LARGE;
    }
    this->m_dead = 0;
    this->m_totalLength = 0.0f;
    this->m_currentLifetime = 0.0f;
    this->m_d8 = 0;
    this->m_fading = 0;
    this->m_enabled = 0;
    this->m_maxWidth = 1.0f;
    this->m_fadeTime = 0.0f;
}

GpuStripEmitter::~GpuStripEmitter() {
    this->m_enabled = 0;
    this->m_lifetime = 0.0f;
    this->m_lastAge = 0.0f;
    this->m_currentLifetime = 0.0f;
    this->m_segments.m_head = 0;
    this->m_segments.m_tail = 0;
    SegmentsSetCapacity(&this->m_segments, 0);
    if (this->m_extra != nullptr) {
        SMemFree(this->m_extra, s_stripFile, 0x111, 0);
        this->m_extra = nullptr;
    }
    if (static_cast<int64_t>(this->m_segments.m_capacity) >= 0) {
        M2FreeFileData(this->m_segments.m_data);
        this->m_segments.m_data = nullptr;
    }
    if (static_cast<int64_t>(this->m_edges.m_capacity) >= 0) {
        M2FreeFileData(this->m_edges.m_data);
        this->m_edges.m_data = nullptr;
    }
}

// ref: FUN_14129c440
void GpuStripEmitter::Reserve(uint32_t count, bool extra) {
    // The edge ring keeps the last three, in the next power of two above.
    int32_t bit = 63;
    while ((3ull >> bit) == 0) {
        bit--;
    }
    uint64_t edges = 1ull << ((bit + 1) & 0x3f);
    RingResize(&this->m_edges, edges);
    RingShrink(&this->m_edges);
    this->m_edges.m_max = 3;
    this->m_edges.m_mask = edges - 1;
    SegmentsSetCapacity(&this->m_segments, count);
    if (extra) {
        this->m_extra = static_cast<float*>(
            SMemAlloc(static_cast<uint64_t>(count) * 0xc, 0x12, s_stripFile, SStrHashFnv1a(s_stripFile), 0x100, 0));
    }
}

// ref: FUN_14129abd0
void GpuStripEmitter::Clear() {
    this->m_enabled = 0;
    this->m_lifetime = 0.0f;
    this->m_lastAge = 0.0f;
    this->m_currentLifetime = 0.0f;
    this->m_segments.m_head = 0;
    this->m_segments.m_tail = 0;
    SegmentsSetCapacity(&this->m_segments, 0);
    if (this->m_extra != nullptr) {
        SMemFree(this->m_extra, s_stripFile, 0x111, 0);
        this->m_extra = nullptr;
    }
}

// ref: FUN_14129abb0
void GpuStripEmitter::Reset() {
    this->m_edges.m_head = 0;
    this->m_edges.m_tail = 0;
    this->m_segments.m_head = 0;
    this->m_segments.m_tail = 0;
}

// ref: FUN_14129c5c0
void GpuStripEmitter::Pop() {
    this->m_edges.m_head--;
    this->m_segments.m_head--;
}

// ref: FUN_14129c5d0
void GpuStripEmitter::Push(const float* top, const float* bottom, const float* extra) {
    if (this->m_enabled == 0 && this->m_currentLifetime == 0.0f) {
        return;
    }
    uint64_t head = this->m_edges.m_head;
    uint64_t count = head + 1 - this->m_edges.m_tail;
    if (this->m_edges.m_max < count) {
        this->m_edges.m_tail = this->m_edges.m_tail - this->m_edges.m_max + count;
    }
    this->m_edges.m_head = head + 1;
    GpuStripEdge* edge = this->m_edges.At(head);
    edge->m_top[0] = top[0];
    edge->m_top[1] = top[1];
    edge->m_top[2] = top[2];
    edge->m_top[3] = 1.0f;
    edge->m_bottom[0] = bottom[0];
    edge->m_bottom[1] = bottom[1];
    edge->m_bottom[2] = bottom[2];
    edge->m_bottom[3] = 1.0f;
    uint64_t segment = this->m_segments.m_head;
    count = segment + 1 - this->m_segments.m_tail;
    if (this->m_segments.m_max < count) {
        this->m_segments.m_tail = this->m_segments.m_tail - this->m_segments.m_max + count;
    }
    this->m_segments.m_head = segment + 1;
    uint64_t slot = segment & this->m_segments.m_mask;
    this->m_segments.m_data[slot].m_age = this->m_currentLifetime;
    if (this->m_extra != nullptr) {
        float* out = this->m_extra + static_cast<uint32_t>(slot) * 3;
        out[0] = extra[0];
        out[1] = extra[1];
        out[2] = extra[2];
    }
    this->Build();
}

void GpuStripEmitter::SetGravity(float gravity) {
    this->m_gravity = gravity;
}

void GpuStripEmitter::SetEnabled(bool enabled) {
    uint8_t on = enabled ? 1 : 0;
    if (this->m_enabled == on) {
        return;
    }
    this->m_enabled = on;
    if (on == 0) {
        if (0.0f < this->m_lifetime) {
            uint64_t head = this->m_segments.m_head;
            uint64_t tail = this->m_segments.m_tail;
            if (head != tail) {
                uint32_t i = static_cast<uint32_t>(head) - static_cast<uint32_t>(tail) - 1;
                float shift = 0.0f;
                for (; i != 0; i--) {
                    shift = this->m_segments.m_data[((this->m_segments.m_mask - i) + head) & this->m_segments.m_mask].m_age;
                    if (0.0f < shift) {
                        break;
                    }
                    shift = 0.0f;
                }
                for (uint64_t j = 0; j != head - tail; j++) {
                    GpuStripSegment* segment =
                        &this->m_segments.m_data[(this->m_segments.m_head - j + this->m_segments.m_mask) & this->m_segments.m_mask];
                    segment->m_age -= shift;
                }
                float lifetime = this->m_currentLifetime - shift;
                if (lifetime <= 0.0f) {
                    lifetime = 0.0f;
                }
                this->m_currentLifetime = lifetime;
                this->m_lastAge = shift;
            }
        }
    } else if (this->m_fading == 0) {
        this->m_currentLifetime = this->m_lifetime;
    }
}

void GpuStripEmitter::SetLifetime(float lifetime) {
    this->m_lifetime = lifetime;
    if (this->m_enabled != 0 && this->m_fading == 0) {
        this->m_currentLifetime = lifetime;
    }
}

void GpuStripEmitter::SetTexLodScale(float scale) {
    this->m_texLodScale = scale;
}

void GpuStripEmitter::SetDetail(uint32_t detail) {
    this->m_detail = detail;
}

bool GpuStripEmitter::CanBlend() const {
    return this->m_d8 != 0 && this->m_extra == nullptr;
}

bool GpuStripEmitter::FindLive(int32_t* index, float* fraction) const {
    *index = 0;
    *fraction = 1.0f;
    uint64_t head = this->m_segments.m_head;
    if (this->m_segments.m_head - this->m_segments.m_tail <= 1) {
        return false;
    }
    int32_t count = static_cast<int32_t>(head) - static_cast<int32_t>(this->m_segments.m_tail);
    if (this->m_lifetime <= 0.0f) {
        *index = count - 1;
        return true;
    }
    int32_t last = count - 1;
    uint64_t mask = this->m_segments.m_mask;
    const GpuStripSegment* data = this->m_segments.m_data;
    for (int32_t i = last; -1 < i; i--) {
        const GpuStripSegment* segment = &data[(mask + head - static_cast<int64_t>(i)) & mask];
        float age = segment->m_age;
        if (i < last) {
            float older = data[(-static_cast<int64_t>(i) + head - 1 + mask) & mask].m_age;
            if (this->CanBlend()) {
                if (0 < i) {
                    segment = &data[((mask - static_cast<int64_t>(i - 1)) + head) & mask];
                }
                older = (age + older) * 0.5f;
                age = (age + segment->m_age) * 0.5f;
            }
            if (0.0f < age) {
                if (older <= 0.0f) {
                    *fraction = age / (age - older);
                }
                *index = i;
                return true;
            }
        } else if (0.0f < age) {
            *index = i;
            return true;
        }
    }
    return false;
}

void GpuStripEmitter::Update(float delta) {
    for (uint32_t i = 0; i < 3; i++) {
        this->m_bounds[i] = FLT_LARGE;
        this->m_bounds[3 + i] = -FLT_LARGE;
    }
    this->m_dead = 0;
    this->m_totalLength = 0.0f;
    this->m_maxWidth = 1.0f;
    uint8_t fading = this->m_fading;
    if (fading != 0) {
        float left = this->m_fadeTime - delta;
        this->m_fadeTime = left;
        if (left <= 0.0f) {
            this->m_fading = 0;
            fading = 0;
            if (this->m_enabled != 0) {
                this->m_currentLifetime = this->m_lifetime;
            }
        }
    }
    if (this->m_enabled == 0) {
        float lifetime = this->m_currentLifetime - delta;
        if (lifetime <= 0.0f) {
            lifetime = 0.0f;
        }
        this->m_currentLifetime = lifetime;
    }
    if (this->m_segments.m_head == this->m_segments.m_tail) {
        return;
    }
    uint64_t mask = this->m_segments.m_mask;
    if (0.0f < this->m_lifetime) {
        uint32_t keep = 1;
        uint32_t start = 0;
        if (this->m_enabled != 0 && fading == 0) {
            start = 1;
        }
        uint32_t last = static_cast<uint32_t>(this->m_segments.m_head) - static_cast<uint32_t>(this->m_segments.m_tail) - 1;
        uint32_t dead = 0;
        for (uint32_t i = start; i <= last; i++) {
            GpuStripSegment* segment = &this->m_segments.m_data[((mask - i) + this->m_segments.m_head) & mask];
            float age = segment->m_age - delta;
            segment->m_age = age;
            dead += age <= 0.0f ? 1 : 0;
        }
        this->m_dead = dead;
        if (this->m_extra == nullptr && this->m_d8 != 0 && dead < this->m_segments.m_head - this->m_segments.m_tail) {
            keep = 2;
        }
        if (keep < dead) {
            uint64_t tail = (dead - keep) + this->m_segments.m_tail;
            this->m_segments.m_tail = tail;
            if (this->m_segments.m_head - tail < this->m_edges.m_head - this->m_edges.m_tail) {
                this->m_edges.m_tail = (tail - this->m_segments.m_head) + this->m_edges.m_head;
            }
            this->m_dead = keep;
        }
    }
    int32_t index = 0;
    float fraction = 1.0f;
    if (this->FindLive(&index, &fraction)) {
        for (uint32_t j = 0; j < static_cast<uint32_t>(index); j++) {
            const GpuStripSegment* segment = &this->m_segments.m_data[((mask - j) + this->m_segments.m_head) & mask];
            this->m_totalLength = segment->m_length + this->m_totalLength;
            float width = segment->m_width;
            if (width <= this->m_maxWidth) {
                width = this->m_maxWidth;
            }
            this->m_maxWidth = width;
            BoxGrow(this->m_bounds, segment->m_points[0][0]);
            BoxGrow(this->m_bounds, segment->m_points[1][0]);
        }
        const GpuStripSegment* segment =
            &this->m_segments.m_data[((this->m_segments.m_head - static_cast<uint32_t>(index)) + mask) & mask];
        this->m_totalLength = fraction * segment->m_length + this->m_totalLength;
        float width = segment->m_width;
        if (width <= this->m_maxWidth) {
            width = this->m_maxWidth;
        }
        this->m_maxWidth = width;
        BoxGrow(this->m_bounds, segment->m_points[0][3]);
        BoxGrow(this->m_bounds, segment->m_points[1][3]);
    }
    if (9.5367432e-07f <= std::fabs(this->m_gravity)) {  // DAT_14261a958
        float drop = this->m_lifetime * this->m_lifetime * this->m_gravity * 1.5f;  // DAT_1426c70a8
        if (drop <= 0.0f) {
            this->m_bounds[2] = drop + this->m_bounds[2];
        } else {
            this->m_bounds[5] = drop + this->m_bounds[5];
        }
    }
}

void GpuStripEmitter::Transform(const float* m) {
    uint64_t head = this->m_edges.m_head;
    uint64_t count = head - this->m_edges.m_tail;
    for (uint64_t j = 0; j != count; j++) {
        GpuStripEdge* edge = &this->m_edges.m_data[((this->m_edges.m_mask - j) + head) & this->m_edges.m_mask];
        TransformPoint(edge->m_top, m);
        TransformPoint(edge->m_bottom, m);
        head = this->m_edges.m_head;
    }
    uint64_t segments = this->m_segments.m_head;
    count = segments - this->m_segments.m_tail;
    for (uint64_t j = 0; j != count; j++) {
        GpuStripSegment* segment =
            &this->m_segments.m_data[((this->m_segments.m_mask - j) + segments) & this->m_segments.m_mask];
        for (uint32_t side = 0; side < 2; side++) {
            float (*p)[4] = segment->m_points[side];
            for (uint32_t k = 0; k < 4; k++) {
                TransformPoint(p[k], m);
            }
            float (*b)[4] = segment->m_basis[side];
            for (uint32_t c = 0; c < 4; c++) {
                float p0 = p[0][c];
                float p1 = p[1][c];
                float p2 = p[2][c];
                float p3 = p[3][c];
                float three1 = 3.0f * p1;  // DAT_142726920
                b[0][c] = p0;
                b[1][c] = -3.0f * p0 + three1;  // DAT_142726930
                b[2][c] = (p2 + p0) * 3.0f + -6.0f * p1;  // DAT_142726940
                b[3][c] = -1.0f * p0 + three1 + -3.0f * p2 + p3;  // DAT_1426d2b20
            }
        }
        segments = this->m_segments.m_head;
    }
}

void GpuStripEmitter::Build() {
    // The fit of the newest segments to the edges (7.8 KB, curve fitting with error terms
    // FUN_14129a670 and FUN_14129bda0) is not ported: segments keep zero points.
    FROST_UNIMPLEMENTED();
}

void GpuStripEmitter::Draw(GxRenderContext* context) {
    // The strip's tessellation and draw (with its device objects FUN_14129c150) are not ported.
    (void)context;
    FROST_UNIMPLEMENTED();
}
