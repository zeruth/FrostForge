#ifndef MODEL2_M2_TRACK_HPP
#define MODEL2_M2_TRACK_HPP

// Animated values (Engine/Source/Model2/M2Animate.cpp): a track's per-instance state, and the
// samplers that find a track's keys at an animation's time and interpolate them. Every sampler
// is one instantiation of a template in the reference; they share the key search, which keeps
// the last key found and walks from it when the time moved by less than 250 ms.

#include <cstdint>

#include "model2/M2Data.hpp"

// The time an animation is at (one of a model's sequence slots).
struct M2SequenceTime {
    uint32_t m_time;            // 0x00 ms into the sequence
    uint16_t m_sequence;        // 0x04 the sequence's index
    uint16_t m_pad06;
    uint32_t m_08;              // 0x08
    uint32_t m_0c;              // 0x0c
    uint32_t m_10;              // 0x10
    float m_speed;              // 0x14 a global sequence's own clock runs while positive
};

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
