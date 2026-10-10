#ifndef MODEL2_M2_DATA_HPP
#define MODEL2_M2_DATA_HPP

// The M2 file format as the engine keeps it in memory (Engine/Source/Model2/M2Data.h). The file
// is read whole and its structures are used in place: M2Init (M2Init.cpp) turns each array's file
// offset into a pointer. An M2Array keeps the file's 8 bytes, a 16-bit count and a 48-bit pointer
// packed after it (the count's high half holds the pointer's top 16 bits). An M2BigArray keeps a
// 32-bit count and a 32-bit offset from its own first byte.

#include <cstdint>

template <class T>
struct M2Array {
    uint16_t m_count;           // 0x00
    uint16_t m_dataHigh;        // 0x02 bits 32-47 of the pointer
    uint32_t m_dataLow;         // 0x04 bits 0-31 (the file offset before M2Init)

    uint32_t Count() const {
        return this->m_count;
    }
    T* Data() const {
        return reinterpret_cast<T*>(static_cast<uintptr_t>(this->m_dataHigh) << 32 | this->m_dataLow);
    }
    T& operator[](uint32_t index) const {
        return this->Data()[index];
    }
    void SetData(const void* data) {
        auto value = reinterpret_cast<uintptr_t>(data);
        this->m_dataLow = static_cast<uint32_t>(value);
        this->m_dataHigh = static_cast<uint16_t>(value >> 32);
    }
};
static_assert(sizeof(M2Array<uint8_t>) == 8, "M2Array is 8 bytes");

template <class T>
struct M2BigArray {
    uint32_t m_count;           // 0x00
    int32_t m_offset;           // 0x04 from this structure's first byte

    uint32_t Count() const {
        return this->m_count;
    }
    T* Data() const {
        return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(this) + static_cast<intptr_t>(this->m_offset));
    }
    T& operator[](uint32_t index) const {
        return this->Data()[index];
    }
};
static_assert(sizeof(M2BigArray<uint8_t>) == 8, "M2BigArray is 8 bytes");

struct C2Vector {
    float x;
    float y;
};

struct C3Vector {
    float x;
    float y;
    float z;
};

struct C4Quaternion {
    float x;
    float y;
    float z;
    float w;
};

struct CAaBox {
    C3Vector m_min;
    C3Vector m_max;
};

// A quaternion packed as four signed 16-bit components.
struct M2CompQuat {
    int16_t x;
    int16_t y;
    int16_t z;
    int16_t w;
};

// A Hermite or Bezier key: the value and its two tangents.
template <class T>
struct M2SplineKey {
    T m_value;
    T m_inTan;
    T m_outTan;
};

// What every animated value starts with (0x0c).
struct M2TrackBase {
    uint16_t m_interpolation;               // 0x00 0 none, 1 linear, 2 bezier, 3 hermite
    int16_t m_globalSequence;               // 0x02 -1 for the model's sequences
    M2Array<M2Array<uint32_t>> m_timestamps; // 0x04 per sequence
};
static_assert(sizeof(M2TrackBase) == 0xc, "M2TrackBase is 0xc bytes");

// An animated value (0x14): keys per sequence.
template <class T>
struct M2Track : M2TrackBase {
    M2Array<M2Array<T>> m_values;           // 0x0c per sequence
};
static_assert(sizeof(M2Track<C3Vector>) == 0x14, "M2Track is 0x14 bytes");

struct M2Loop {
    uint32_t m_timestamp;
};

struct M2Bounds {
    CAaBox m_extent;                        // 0x00
    float m_radius;                         // 0x18
};
static_assert(sizeof(M2Bounds) == 0x1c, "M2Bounds is 0x1c bytes");

struct M2Sequence {
    uint16_t m_id;                          // 0x00
    uint16_t m_variationIndex;              // 0x02
    uint32_t m_duration;                    // 0x04
    float m_moveSpeed;                      // 0x08
    // 0x20 looped, 0x40 in an .anim file, 0x2000 ... (0x2020 with 0x20 set: data in this file)
    uint32_t m_flags;                       // 0x0c
    int16_t m_frequency;                    // 0x10
    uint16_t m_pad12;
    uint32_t m_replayMin;                   // 0x14
    uint32_t m_replayMax;                   // 0x18
    uint16_t m_blendTimeIn;                 // 0x1c
    uint16_t m_blendTimeOut;                // 0x1e
    M2Bounds m_bounds;                      // 0x20
    int16_t m_variationNext;                // 0x3c
    uint16_t m_aliasNext;                   // 0x3e
};
static_assert(sizeof(M2Sequence) == 0x40, "M2Sequence is 0x40 bytes");

struct M2CompBone {
    int32_t m_keyBoneId;                    // 0x00
    // 0x200 transformed, 0x8 spherical billboard, ... 0x70000 billboard kinds, 0x100000 used by a
    // skin section
    uint32_t m_flags;                       // 0x04
    int16_t m_parentBone;                   // 0x08
    uint16_t m_submeshId;                   // 0x0a
    uint32_t m_boneNameCrc;                 // 0x0c
    M2Track<C3Vector> m_translation;        // 0x10
    M2Track<M2CompQuat> m_rotation;         // 0x24
    M2Track<C3Vector> m_scale;              // 0x38
    C3Vector m_pivot;                       // 0x4c
};
static_assert(sizeof(M2CompBone) == 0x58, "M2CompBone is 0x58 bytes");

struct M2Vertex {
    C3Vector m_position;                    // 0x00
    uint8_t m_boneWeights[4];               // 0x0c
    uint8_t m_boneIndices[4];               // 0x10
    C3Vector m_normal;                      // 0x14
    C2Vector m_texCoords[2];                // 0x20
};
static_assert(sizeof(M2Vertex) == 0x30, "M2Vertex is 0x30 bytes");

struct M2Color {
    M2Track<C3Vector> m_color;              // 0x00
    M2Track<uint16_t> m_alpha;              // 0x14 fixed 16
};
static_assert(sizeof(M2Color) == 0x28, "M2Color is 0x28 bytes");

struct M2Texture {
    uint32_t m_type;                        // 0x00 0 file, otherwise replaceable
    uint32_t m_flags;                       // 0x04
    M2Array<char> m_filename;               // 0x08
};
static_assert(sizeof(M2Texture) == 0x10, "M2Texture is 0x10 bytes");

struct M2TextureWeight {
    M2Track<uint16_t> m_weight;             // 0x00
};
static_assert(sizeof(M2TextureWeight) == 0x14, "M2TextureWeight is 0x14 bytes");

struct M2TextureTransform {
    M2Track<C3Vector> m_translation;        // 0x00
    M2Track<C4Quaternion> m_rotation;       // 0x14
    M2Track<C3Vector> m_scale;              // 0x28
};
static_assert(sizeof(M2TextureTransform) == 0x3c, "M2TextureTransform is 0x3c bytes");

struct M2Material {
    uint16_t m_flags;                       // 0x00 1 unlit (set too for blend modes above 4), ...
    uint16_t m_blendMode;                   // 0x02
};
static_assert(sizeof(M2Material) == 4, "M2Material is 4 bytes");

struct M2Attachment {
    uint32_t m_id;                          // 0x00
    uint16_t m_bone;                        // 0x04
    uint16_t m_pad06;
    C3Vector m_position;                    // 0x08
    M2Track<uint8_t> m_animate;             // 0x14
};
static_assert(sizeof(M2Attachment) == 0x28, "M2Attachment is 0x28 bytes");

struct M2Event {
    uint32_t m_id;                          // 0x00 a four-character code
    uint32_t m_data;                        // 0x04
    uint16_t m_bone;                        // 0x08
    uint16_t m_pad0a;
    C3Vector m_position;                    // 0x0c
    M2TrackBase m_enabled;                  // 0x18
};
static_assert(sizeof(M2Event) == 0x24, "M2Event is 0x24 bytes");

struct M2Light {
    uint16_t m_type;                        // 0x00
    int16_t m_bone;                         // 0x02
    C3Vector m_position;                    // 0x04
    M2Track<C3Vector> m_ambientColor;       // 0x10
    M2Track<float> m_ambientIntensity;      // 0x24
    M2Track<C3Vector> m_diffuseColor;       // 0x38
    M2Track<float> m_diffuseIntensity;      // 0x4c
    M2Track<float> m_attenuationStart;      // 0x60
    M2Track<float> m_attenuationEnd;        // 0x74
    M2Track<uint8_t> m_visibility;          // 0x88
};
static_assert(sizeof(M2Light) == 0x9c, "M2Light is 0x9c bytes");

struct M2Camera {
    uint32_t m_type;                        // 0x00
    float m_farClip;                        // 0x04
    float m_nearClip;                       // 0x08
    M2Track<M2SplineKey<C3Vector>> m_positions;         // 0x0c
    C3Vector m_positionBase;                            // 0x20
    M2Track<M2SplineKey<C3Vector>> m_targetPositions;   // 0x2c
    C3Vector m_targetPositionBase;                      // 0x40
    M2Track<M2SplineKey<float>> m_roll;                 // 0x4c
    M2Track<M2SplineKey<float>> m_fieldOfView;          // 0x60
};
static_assert(sizeof(M2Camera) == 0x74, "M2Camera is 0x74 bytes");

struct M2Ribbon {
    uint32_t m_ribbonId;                    // 0x00
    uint32_t m_boneIndex;                   // 0x04
    C3Vector m_position;                    // 0x08
    M2Array<uint16_t> m_textureIndices;     // 0x14
    M2Array<uint16_t> m_materialIndices;    // 0x1c
    M2Track<C3Vector> m_color;              // 0x24
    M2Track<uint16_t> m_alpha;              // 0x38
    M2Track<float> m_heightAbove;           // 0x4c
    M2Track<float> m_heightBelow;           // 0x60
    float m_edgesPerSecond;                 // 0x74
    float m_edgeLifetime;                   // 0x78
    float m_gravity;                        // 0x7c
    uint16_t m_textureRows;                 // 0x80
    uint16_t m_textureCols;                 // 0x82
    M2Track<uint16_t> m_textureSlot;        // 0x84
    M2Track<uint8_t> m_visibility;          // 0x98
    int16_t m_priorityPlane;                // 0xac
    uint8_t m_ribbonColorIndex;             // 0xae
    uint8_t m_textureTransformLookupIndex;  // 0xaf
};
static_assert(sizeof(M2Ribbon) == 0xb0, "M2Ribbon is 0xb0 bytes");

// A track kept outside the sequences: keys and values without timing per sequence.
template <class T>
struct M2PartTrack {
    M2Array<uint16_t> m_times;              // 0x00 fixed 16
    M2Array<T> m_values;                    // 0x08
};

struct M2Particle {
    uint32_t m_particleId;                  // 0x00
    uint32_t m_flags;                       // 0x04
    C3Vector m_position;                    // 0x08
    uint16_t m_bone;                        // 0x14
    uint16_t m_texture;                     // 0x16 (or three packed indices)
    M2Array<char> m_geometryModelFilename;  // 0x18
    M2Array<char> m_recursionModelFilename; // 0x20
    uint8_t m_blendingType;                 // 0x28
    uint8_t m_emitterType;                  // 0x29
    uint16_t m_particleColorIndex;          // 0x2a
    uint8_t m_particleType;                 // 0x2c
    uint8_t m_headOrTail;                   // 0x2d
    uint16_t m_textureTileRotation;         // 0x2e
    uint16_t m_textureDimensionsRows;       // 0x30
    uint16_t m_textureDimensionsColumns;    // 0x32
    M2Track<float> m_emissionSpeed;         // 0x34
    M2Track<float> m_speedVariation;        // 0x48
    M2Track<float> m_verticalRange;         // 0x5c
    M2Track<float> m_horizontalRange;       // 0x70
    M2Track<float> m_gravity;               // 0x84
    M2Track<float> m_lifespan;              // 0x98
    float m_lifespanVary;                   // 0xac
    M2Track<float> m_emissionRate;          // 0xb0
    float m_emissionRateVary;               // 0xc4
    M2Track<float> m_emissionAreaLength;    // 0xc8
    M2Track<float> m_emissionAreaWidth;     // 0xdc
    M2Track<float> m_zSource;               // 0xf0
    M2PartTrack<C3Vector> m_colorTrack;     // 0x104
    M2PartTrack<uint16_t> m_alphaTrack;     // 0x114
    M2PartTrack<C2Vector> m_scaleTrack;     // 0x124
    C2Vector m_scaleVary;                   // 0x134
    M2PartTrack<uint16_t> m_headCellTrack;  // 0x13c
    M2PartTrack<uint16_t> m_tailCellTrack;  // 0x14c
    float m_tailLength;                     // 0x15c
    float m_twinkleSpeed;                   // 0x160
    float m_twinklePercent;                 // 0x164
    float m_twinkleScaleMin;                // 0x168
    float m_twinkleScaleMax;                // 0x16c
    float m_burstMultiplier;                // 0x170
    float m_drag;                           // 0x174
    float m_baseSpin;                       // 0x178
    float m_baseSpinVary;                   // 0x17c
    float m_spin;                           // 0x180
    float m_spinVary;                       // 0x184
    float m_tumble[6];                      // 0x188 min xyz, max xyz
    C3Vector m_windVector;                  // 0x1a0
    float m_windTime;                       // 0x1ac
    float m_followSpeed1;                   // 0x1b0
    float m_followScale1;                   // 0x1b4
    float m_followSpeed2;                   // 0x1b8
    float m_followScale2;                   // 0x1bc
    M2Array<C3Vector> m_splinePoints;       // 0x1c0
    M2Track<uint8_t> m_enabledIn;           // 0x1c8
    uint8_t m_multiTextureParams[0x10];     // 0x1dc
};
static_assert(sizeof(M2Particle) == 0x1ec, "M2Particle is 0x1ec bytes");

// The EXP2 chunk's per-emitter extras (0x1c).
struct M2ExtendedParticle {
    float m_zSource;                        // 0x00
    float m_colorMult;                      // 0x04
    float m_alphaMult;                      // 0x08
    M2Array<uint16_t> m_alphaCutoffTimes;   // 0x0c
    M2Array<uint16_t> m_alphaCutoffValues;  // 0x14
};
static_assert(sizeof(M2ExtendedParticle) == 0x1c, "M2ExtendedParticle is 0x1c bytes");

// The MD20 header (M2Data, 0x130).
struct M2Data {
    uint32_t m_magic;                                   // 0x000 'MD20'
    uint32_t m_version;                                 // 0x004 0x110..0x112
    M2Array<char> m_name;                               // 0x008
    // 8 blend map overrides, 0x80 ..., 0x100000 skeleton in a .skel file (SKID), 0x800000,
    // 0x1000000 no physics
    uint32_t m_flags;                                   // 0x010
    M2Array<M2Loop> m_loops;                            // 0x014
    M2Array<M2Sequence> m_sequences;                    // 0x01c
    M2Array<uint16_t> m_sequenceIdxHashById;            // 0x024
    M2Array<M2CompBone> m_bones;                        // 0x02c
    M2Array<uint16_t> m_boneIndicesById;                // 0x034
    M2BigArray<M2Vertex> m_vertices;                    // 0x03c
    uint32_t m_numSkinProfiles;                         // 0x044
    M2Array<M2Color> m_colors;                          // 0x048
    M2Array<M2Texture> m_textures;                      // 0x050
    M2Array<M2TextureWeight> m_textureWeights;          // 0x058
    M2Array<M2TextureTransform> m_textureTransforms;    // 0x060
    M2Array<uint16_t> m_textureIndicesById;             // 0x068
    M2Array<M2Material> m_materials;                    // 0x070
    M2Array<uint16_t> m_boneCombos;                     // 0x078
    M2Array<uint16_t> m_textureCombos;                  // 0x080
    M2Array<uint16_t> m_textureTransformBoneMap;        // 0x088
    M2Array<uint16_t> m_textureWeightCombos;            // 0x090
    M2Array<uint16_t> m_textureTransformCombos;         // 0x098
    CAaBox m_bounds;                                    // 0x0a0
    float m_boundsRadius;                               // 0x0b8
    CAaBox m_collisionBounds;                           // 0x0bc
    float m_collisionRadius;                            // 0x0d4
    M2Array<uint16_t> m_collisionIndices;               // 0x0d8
    M2Array<C3Vector> m_collisionPositions;             // 0x0e0
    M2Array<C3Vector> m_collisionFaceNormals;           // 0x0e8
    M2Array<M2Attachment> m_attachments;                // 0x0f0
    M2Array<uint16_t> m_attachmentIndicesById;          // 0x0f8
    M2Array<M2Event> m_events;                          // 0x100
    M2Array<M2Light> m_lights;                          // 0x108
    M2Array<M2Camera> m_cameras;                        // 0x110
    M2Array<uint16_t> m_cameraIndicesById;              // 0x118
    M2Array<M2Ribbon> m_ribbons;                        // 0x120
    M2Array<M2Particle> m_particles;                    // 0x128
};
static_assert(sizeof(M2Data) == 0x130, "M2Data is 0x130 bytes");

// ---- the skin profile (.skin) ----------------------------------------------------------------

struct M2SkinSection {
    uint16_t m_skinSectionId;               // 0x00
    uint16_t m_indexStartHigh;              // 0x02 bits 16-31 of the first index
    uint16_t m_vertexStart;                 // 0x04
    uint16_t m_vertexCount;                 // 0x06
    uint16_t m_indexStart;                  // 0x08
    uint16_t m_indexCount;                  // 0x0a
    uint16_t m_boneCount;                   // 0x0c
    uint16_t m_boneComboIndex;              // 0x0e
    uint8_t m_boneInfluences;               // 0x10
    uint8_t m_indexCountHigh;               // 0x11 bits 16-23 of the index count
    uint16_t m_centerBoneIndex;             // 0x12
    C3Vector m_centerPosition;              // 0x14
    C3Vector m_sortCenterPosition;          // 0x20
    float m_sortRadius;                     // 0x2c

    uint32_t IndexStart() const {
        return static_cast<uint32_t>(this->m_indexStartHigh) << 16 | this->m_indexStart;
    }
    uint32_t IndexCount() const {
        return static_cast<uint32_t>(this->m_indexCountHigh) << 16 | this->m_indexCount;
    }
};
static_assert(sizeof(M2SkinSection) == 0x30, "M2SkinSection is 0x30 bytes");

struct M2Batch {
    uint8_t m_flags;                        // 0x00 4 projected
    int8_t m_priorityPlane;                 // 0x01
    uint16_t m_shaderId;                    // 0x02 (0x8000 set: an M2Effect index)
    uint16_t m_skinSectionIndex;            // 0x04
    uint16_t m_geosetIndex;                 // 0x06 (bit 1 of its high byte: projected)
    uint16_t m_colorIndex;                  // 0x08
    uint16_t m_materialIndex;               // 0x0a
    uint16_t m_materialLayer;               // 0x0c
    uint16_t m_textureCount;                // 0x0e
    uint16_t m_textureComboIndex;           // 0x10
    uint16_t m_textureCoordComboIndex;      // 0x12
    uint16_t m_textureWeightComboIndex;     // 0x14
    uint16_t m_textureTransformComboIndex;  // 0x16
};
static_assert(sizeof(M2Batch) == 0x18, "M2Batch is 0x18 bytes");

struct M2ShadowBatch {
    uint8_t m_flags;                        // 0x00
    uint8_t m_flags2;                       // 0x01
    uint16_t m_unknown1;                    // 0x02
    uint16_t m_submeshId;                   // 0x04
    uint16_t m_textureId;                   // 0x06
    uint16_t m_colorId;                     // 0x08
    uint16_t m_transparencyId;              // 0x0a
};
static_assert(sizeof(M2ShadowBatch) == 0xc, "M2ShadowBatch is 0xc bytes");

struct M2SkinProfile {
    uint32_t m_magic;                           // 0x00 'SKIN'
    M2BigArray<uint16_t> m_vertices;            // 0x04
    M2BigArray<uint16_t> m_indices;             // 0x0c
    M2BigArray<uint32_t> m_bones;               // 0x14
    M2Array<M2SkinSection> m_skinSections;      // 0x1c
    M2Array<M2Batch> m_batches;                 // 0x24
    uint32_t m_globalVertexOffset;              // 0x2c the first vertex (M2Data flag 0x2000)
    M2Array<M2ShadowBatch> m_shadowBatches;     // 0x30
};
static_assert(sizeof(M2SkinProfile) == 0x38, "M2SkinProfile is 0x38 bytes");

#endif
