#ifndef MODEL2_GPU_STRIP_EMITTER_HPP
#define MODEL2_GPU_STRIP_EMITTER_HPP

// GpuStripEmitter (Engine/Source/Services/GpuStripEmitter.cpp, 0xe0): a strip laid down edge by
// edge, kept as a short ring of the last edges and a longer ring of curve segments, each two
// cubic Beziers (the strip's two sides) fitted through the edges with their power-basis
// coefficients, and aged out at the tail. The ribbons are its only user.

#include <cstddef>
#include <cstdint>

class GxRenderContext;

// An edge (0x20): the strip's two sides where it was laid down, w 1.
struct GpuStripEdge {
    float m_top[4];
    float m_bottom[4];
};
static_assert(sizeof(GpuStripEdge) == 0x20, "GpuStripEdge is 0x20 bytes");

// A segment (0x110): each side's four Bezier control points, then their power-basis
// coefficients, its length and width, and how long it has left.
struct GpuStripSegment {
    float m_points[2][4][4];            // 0x000
    float m_basis[2][4][4];             // 0x080
    float m_length;                     // 0x100
    float m_width;                      // 0x104
    float m_age;                        // 0x108 the life it has left
    uint32_t m_pad10c;
};
static_assert(sizeof(GpuStripSegment) == 0x110, "GpuStripSegment is 0x110 bytes");

// A ring over an aligned growable array: its storage, size and capacity (bit 63 set when it is
// not owned), the most it keeps, the index mask, and the running head and tail.
template <class T>
struct GpuStripRing {
    T* m_data;                          // 0x00
    uint64_t m_size;                    // 0x08
    uint64_t m_capacity;                // 0x10
    uint64_t m_max;                     // 0x18
    uint64_t m_mask;                    // 0x20
    uint64_t m_head;                    // 0x28
    uint64_t m_tail;                    // 0x30

    T* At(uint64_t index) const {
        return &this->m_data[index & this->m_mask];
    }
    uint64_t Count() const {
        return this->m_head - this->m_tail;
    }
};

class GpuStripEmitter {
    public:
    GpuStripRing<GpuStripEdge> m_edges;         // 0x00
    GpuStripRing<GpuStripSegment> m_segments;   // 0x38
    float* m_extra;                     // 0x70 three floats per segment, when asked for
    int64_t m_78;                       // 0x78 -1
    float m_lifetime;                   // 0x80 how long a segment lives
    float m_lastAge;                    // 0x84
    float m_gravity;                    // 0x88
    float m_texLodScale;                // 0x8c 1
    uint32_t m_detail;                  // 0x90 1..4 strips across
    float m_bounds[6];                  // 0x94 around the live segments
    float m_drawBounds[6];              // 0xac around what the last draw built
    uint32_t m_dead;                    // 0xc4 segments past their age
    float m_totalLength;                // 0xc8
    float m_maxWidth;                   // 0xcc 1
    float m_fadeTime;                   // 0xd0
    float m_currentLifetime;            // 0xd4 what new segments get
    uint8_t m_d8;                       // 0xd8
    uint8_t m_fading;                   // 0xd9
    uint8_t m_enabled;                  // 0xda
    uint8_t m_padDb[5];

    // ref: FUN_141298340
    GpuStripEmitter();
    // ref: FUN_141298440
    ~GpuStripEmitter();
    // Room for `count` segments, and the extra floats when `extra` (FUN_14129c440).
    void Reserve(uint32_t count, bool extra);
    // Every segment dropped and the storage given back (FUN_14129abd0).
    void Clear();
    // The rings emptied (FUN_14129abb0).
    void Reset();
    // The last edge and its segment taken back (FUN_14129c5c0).
    void Pop();
    // An edge laid down and the newest segment fitted to it (FUN_14129c5d0).
    void Push(const float* top, const float* bottom, const float* extra);
    // ref: FUN_14129c740
    void SetGravity(float gravity);
    // On or off; switched off, the segments' ages are rebased so the strip ends where it stopped
    // (FUN_14129c750).
    void SetEnabled(bool enabled);
    // ref: FUN_14129c880
    void SetLifetime(float lifetime);
    // ref: FUN_14129c8b0
    void SetTexLodScale(float scale);
    // ref: FUN_14129c8c0
    void SetDetail(uint32_t detail);
    // The frame: the ages run down, dead segments dropped and the bounds rebuilt (FUN_14129c950).
    void Update(float delta);
    // The edges and segments moved through a matrix (FUN_1412984e0).
    void Transform(const float* matrix);
    // The segments fitted to the newest edges (FUN_1412987e0).
    void Build();
    // The strip drawn (FUN_14129ac40).
    void Draw(GxRenderContext* context);
    // The newest segment still alive and how much of it shows (FUN_14129bf00).
    bool FindLive(int32_t* index, float* fraction) const;
    // Whether segments may be blended at their ends: no extra floats and flag 0xd8
    // (FUN_14129c5a0).
    bool CanBlend() const;
};
static_assert(offsetof(GpuStripEmitter, m_extra) == 0x70, "GpuStripEmitter extra at 0x70");
static_assert(offsetof(GpuStripEmitter, m_bounds) == 0x94, "GpuStripEmitter bounds at 0x94");
static_assert(offsetof(GpuStripEmitter, m_dead) == 0xc4, "GpuStripEmitter dead count at 0xc4");
static_assert(offsetof(GpuStripEmitter, m_enabled) == 0xda, "GpuStripEmitter enabled at 0xda");
static_assert(sizeof(GpuStripEmitter) == 0xe0, "GpuStripEmitter is 0xe0 bytes");

#endif
