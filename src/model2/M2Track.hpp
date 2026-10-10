#ifndef MODEL2_M2_TRACK_HPP
#define MODEL2_M2_TRACK_HPP

// Animated values (Engine/Source/Model2/M2Animate.cpp): a track's per-instance state, and the
// samplers that find a track's keys at an animation's time and interpolate them. Every sampler
// is one instantiation of a template in the reference; they share the key search, which keeps
// the last key found and walks from it when the time moved by less than 250 ms.

#include <cstdint>

#include "model2/M2Data.hpp"

// Where a sequence's bones and sequences come from: the model's own file or a parent skeleton
// (0x34; FUN_1412006d0 copies one, FUN_141200740 clears it).
struct M2SequenceSource {
    M2Array<M2CompBone>* m_bones;           // 0x00
    M2Array<uint16_t>* m_keyBoneLookup;     // 0x08 bone index by key bone id
    M2Array<M2Sequence>* m_sequences;       // 0x10
    M2Array<uint16_t>* m_sequenceHash;      // 0x18
    M2Array<M2Attachment>* m_attachments;   // 0x20
    M2Array<uint16_t>* m_attachmentLookup;  // 0x28
    uint32_t m_flags;                       // 0x30 1 the parent's
    uint32_t m_34;                          // 0x34
};
static_assert(sizeof(M2SequenceSource) == 0x38, "M2SequenceSource is 0x38 bytes with its tail");

// One animation playing on a model's bones ("M2 Bone Seq Pool", 0x68): the time it is at and
// how that time advances.
struct M2SequenceTime {
    uint32_t m_time;            // 0x00 ms into the sequence
    uint16_t m_sequence;        // 0x04 the sequence's index
    uint16_t m_bone;            // 0x06 the bone that last advanced it
    uint16_t m_nextSequence;    // 0x08 0xffff for none
    uint8_t m_pending;          // 0x0a ended, or waiting to start
    uint8_t m_endless;          // 0x0b played until replaced
    int32_t m_startTime;        // 0x0c the clock when it started
    int32_t m_endTime;          // 0x10
    float m_speed;              // 0x14 a global sequence's own clock runs while positive
    float m_inverseSpeed;       // 0x18 1 / speed, 0 when stopped
    uint32_t m_timeOffset;      // 0x1c
    uint32_t m_repeats;         // 0x20
    uint32_t m_24;              // 0x24
    M2SequenceSource m_source;  // 0x28 (its flags at 0x58)
    uint64_t m_60;              // 0x60
};
static_assert(sizeof(M2SequenceTime) == 0x68, "M2SequenceTime is 0x68 bytes");

// A sequence id resolved through the animation DB (FUN_1411ff0e0): the id to play, how it
// was found (0 itself, 1 reversed, 2.. held), whether the chain asked for a blend, whether it
// fell back to a default, and whether only the parent skeleton has it.
struct M2SequenceResolve {
    uint16_t m_sequence;                    // 0x00
    uint8_t m_kind;                         // 0x02
    uint8_t m_blend;                        // 0x03
    uint8_t m_fallback;                     // 0x04
    uint8_t m_parentHas;                    // 0x05
};
static_assert(sizeof(M2SequenceResolve) == 6, "M2SequenceResolve is 6 bytes");

// What a sampler reads besides the track: the primary and blended-from sequences and the blend
// weight, at the offsets every animated element keeps them (a bone's state is the full 0x90).
struct M2AnimTarget {
    uint8_t m_00[0x40];
    M2SequenceTime* m_sequence;         // 0x40
    M2SequenceTime* m_blendSequence;    // 0x48
    uint8_t m_50[0x2c];
    float m_blendWeight;                // 0x7c 0 when not blending
};

// A track's state: the last keys found (primary, blended-from) and the value.
template <class T>
struct M2TrackState {
    uint32_t m_key;             // 0x00
    uint32_t m_blendKey;        // 0x04
    T m_value;                  // 0x08
};

// The sampler flags: 2 skips the primary sequence, 4 the blended-from one.
enum : uint32_t {
    M2TRACK_SKIP_PRIMARY = 0x2,
    M2TRACK_SKIP_BLEND = 0x4,
};

// A state's starting value: the first key of the first sequence, or `def` (FUN_1411fe990 u8,
// FUN_1411fe9d0 u16, FUN_1411fea10 32 bits, FUN_1411fea50 a packed quaternion, FUN_1411feb00 a
// vector, FUN_1411feb50 16 bytes, FUN_1411feb90 fixed 16 as a float).
void M2TrackInit(const M2Track<uint8_t>* track, M2TrackState<uint8_t>* state, const uint8_t* def);
void M2TrackInit(const M2Track<uint16_t>* track, M2TrackState<uint16_t>* state, const uint16_t* def);
void M2TrackInit(const M2Track<float>* track, M2TrackState<float>* state, const float* def);
void M2TrackInit(const M2Track<M2CompQuat>* track, M2TrackState<C4Quaternion>* state, const C4Quaternion* def);
void M2TrackInit(const M2Track<C3Vector>* track, M2TrackState<C3Vector>* state, const C3Vector* def);
void M2TrackInit(const M2Track<C4Quaternion>* track, M2TrackState<C4Quaternion>* state, const C4Quaternion* def);
void M2TrackInitFixed16(const M2Track<uint16_t>* track, M2TrackState<float>* state, const float* def);

// The samplers. `globalTimes` is the model's clock per global sequence; `blendTrack` is the
// track the blended-from sequence reads (the same track for every caller seen).
// FUN_1411fbee0: a spline vector (camera positions), linear, Bezier or Hermite.
void M2TrackSample(const uint32_t* globalTimes, const M2AnimTarget* target,
    const M2Track<M2SplineKey<C3Vector>>* track, const M2Track<M2SplineKey<C3Vector>>* blendTrack,
    M2TrackState<C3Vector>* state, const C3Vector* def, uint32_t flags);
// FUN_1411fb790: a spline float (camera roll and field of view).
void M2TrackSample(const uint32_t* globalTimes, const M2AnimTarget* target,
    const M2Track<M2SplineKey<float>>* track, const M2Track<M2SplineKey<float>>* blendTrack,
    M2TrackState<float>* state, const float* def, uint32_t flags);
// FUN_1411fc8e0: a byte, stepped; the blended-from key is tracked but not blended.
void M2TrackSample(const uint32_t* globalTimes, const M2AnimTarget* target, const M2Track<uint8_t>* track,
    const M2Track<uint8_t>* blendTrack, M2TrackState<uint8_t>* state, const uint8_t* def, uint32_t flags);
// FUN_1411fccc0: a float.
void M2TrackSample(const uint32_t* globalTimes, const M2AnimTarget* target, const M2Track<float>* track,
    const M2Track<float>* blendTrack, M2TrackState<float>* state, const float* def, uint32_t flags);
// FUN_1411fd1d0: a packed quaternion.
void M2TrackSample(const uint32_t* globalTimes, const M2AnimTarget* target, const M2Track<M2CompQuat>* track,
    const M2Track<M2CompQuat>* blendTrack, M2TrackState<C4Quaternion>* state, const C4Quaternion* def,
    uint32_t flags);
// FUN_1411fd910: a vector.
void M2TrackSample(const uint32_t* globalTimes, const M2AnimTarget* target, const M2Track<C3Vector>* track,
    const M2Track<C3Vector>* blendTrack, M2TrackState<C3Vector>* state, const C3Vector* def, uint32_t flags);
// FUN_1411fdf00: a quaternion.
void M2TrackSample(const uint32_t* globalTimes, const M2AnimTarget* target, const M2Track<C4Quaternion>* track,
    const M2Track<C4Quaternion>* blendTrack, M2TrackState<C4Quaternion>* state, const C4Quaternion* def,
    uint32_t flags);
// FUN_1411fe450: fixed 16 as a float.
void M2TrackSampleFixed16(const uint32_t* globalTimes, const M2AnimTarget* target, const M2Track<uint16_t>* track,
    const M2Track<uint16_t>* blendTrack, M2TrackState<float>* state, const float* def, uint32_t flags);

// Quaternions (NTempest C4Quaternion): a normalised lerp (FUN_141348580) and a slerp
// (FUN_1413486d0).
C4Quaternion* QuaternionNlerp(C4Quaternion* out, float t, const C4Quaternion* a, const C4Quaternion* b);
C4Quaternion* QuaternionSlerp(C4Quaternion* out, float t, const C4Quaternion* a, const C4Quaternion* b);

#endif
