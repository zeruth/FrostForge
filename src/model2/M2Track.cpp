#include "model2/M2Track.hpp"

#include <cmath>
#include <cstring>

namespace {

// DAT_1427228e0: a packed quaternion component's scale (1 / 32767.5).
constexpr float kQuatScale = 1.0f / 32767.5f;
// DAT_1427228e4: fixed 16's scale (1 / 32767).
constexpr float kFixed16Scale = 1.0f / 32767.0f;

uint16_t PackedU16(const int16_t value) {
    return static_cast<uint16_t>(value);
}

C4Quaternion UnpackQuat(const M2CompQuat& q) {
    return C4Quaternion{
        static_cast<float>(PackedU16(q.x)) * kQuatScale - 1.0f,
        static_cast<float>(PackedU16(q.y)) * kQuatScale - 1.0f,
        static_cast<float>(PackedU16(q.z)) * kQuatScale - 1.0f,
        static_cast<float>(PackedU16(q.w)) * kQuatScale - 1.0f,
    };
}

// The key search every sampler inlines. `cached` is the state's last key; on return `k0` and
// `k1` are the keys around `time` (equal at either end) and `t` how far between them.
void FindKeys(const M2Array<uint32_t>& times, uint32_t time, uint32_t* cached, uint32_t& k0, uint32_t& k1,
    float& t) {
    uint32_t count = times.Count();
    t = 0.0f;
    if (count < 2) {
        *cached = 0;
        k0 = 0;
        k1 = 0;
        return;
    }
    uint32_t k = *cached;
    if (count <= k) {
        *cached = 0;
        k = 0;
    }
    const uint32_t* keys = times.Data();
    if (time - keys[k] < 0xfa) {
        // A little ahead of the last key: walk forward.
        for (;;) {
            k0 = k;
            if (count - 1 <= k) {
                break;
            }
            k++;
            if (time < keys[k]) {
                break;
            }
        }
    } else if (time - keys[k] < 0xffffff06) {
        // Far away: search again.
        k0 = 0;
        if (time < 0xfa) {
            if (count != 1) {
                uint32_t lo = 0;
                uint32_t next;
                do {
                    k0 = lo;
                    next = lo + 1;
                    if (time < keys[next]) {
                        break;
                    }
                    k0 = next;
                    lo = next;
                } while (next < count - 1);
            }
        } else {
            uint32_t lo = 0;
            uint32_t hi = count;
            do {
                uint32_t mid = (hi + lo) >> 1;
                k0 = mid;
                if (time < keys[mid]) {
                    if (mid == 0) {
                        *cached = 0;
                        k0 = 0;
                        k1 = 0;
                        return;
                    }
                    hi = mid - 1;
                } else {
                    if (count <= mid + 1 || time < keys[mid + 1]) {
                        break;
                    }
                    lo = mid + 1;
                }
                k0 = lo;
            } while (k0 < hi);
        }
    } else {
        // A little behind: walk back.
        for (;;) {
            k0 = k;
            if (k == 0 || time >= keys[k]) {
                break;
            }
            k--;
        }
    }
    *cached = k0;
    k1 = k0 + 1;
    if (k1 < count) {
        t = static_cast<float>(time - keys[k0]) / static_cast<float>(keys[k1] - keys[k0]);
    } else {
        k1 = k0;
        t = 0.0f;
    }
}

// A sequence's timestamps and time: a global sequence runs on the model's clock.
void SequenceTimes(const uint32_t* globalTimes, const M2TrackBase* track, const M2SequenceTime* seq,
    uint32_t& tsIndex, uint32_t& time) {
    time = seq->m_time;
    tsIndex = seq->m_sequence;
    if (static_cast<uint16_t>(track->m_globalSequence) == 0xffff) {
        if (track->m_timestamps.Count() <= seq->m_sequence) {
            tsIndex = 0;
        }
    } else {
        tsIndex = 0;
        if (0.0f < seq->m_speed) {
            time = globalTimes[static_cast<uint16_t>(track->m_globalSequence)];
        }
    }
}

// Finds the keys of `track` for `seq` into `cached`/`k0`/`k1`/`t` (all 0 when the track has no
// timestamps).
template <class T>
void SequenceKeys(const uint32_t* globalTimes, const M2Track<T>* track, const M2SequenceTime* seq,
    uint32_t* cached, uint32_t& k0, uint32_t& k1, float& t) {
    if (track->m_timestamps.Count() == 0) {
        *cached = 0;
        k0 = 0;
        k1 = 0;
        t = 0.0f;
        return;
    }
    uint32_t tsIndex;
    uint32_t time;
    SequenceTimes(globalTimes, track, seq, tsIndex, time);
    FindKeys(track->m_timestamps[tsIndex], time, cached, k0, k1, t);
}

// The values index of a sequence: its own, or the first when the track has fewer.
template <class T>
uint32_t ValuesIndex(const M2Track<T>* track, const M2SequenceTime* seq) {
    if (seq == nullptr || track->m_values.Count() <= seq->m_sequence) {
        return 0;
    }
    return seq->m_sequence;
}

C3Vector Lerp(const C3Vector& a, const C3Vector& b, float t) {
    return C3Vector{(b.x - a.x) * t + a.x, (b.y - a.y) * t + a.y, (b.z - a.z) * t + a.z};
}

float Lerp(float a, float b, float t) {
    return (b - a) * t + a;
}

// DAT_14262f25c, DAT_1426c8878
constexpr float kThree = 3.0f;
constexpr float kSix = 6.0f;

C3Vector SplineValue(uint16_t interpolation, const M2SplineKey<C3Vector>& a, const M2SplineKey<C3Vector>& b,
    float t) {
    if (interpolation == 2) {
        // Bezier.
        float t2 = t * t;
        float t3 = t2 * t;
        float wInB = t2 * kThree - t3 * kThree;
        float wOutA = (t3 * kThree - t2 * kSix) + t * kThree;
        float wA = ((t2 * kThree - t3) - t * kThree) + 1.0f;
        return C3Vector{
            wOutA * a.m_outTan.x + wA * a.m_value.x + wInB * b.m_inTan.x + t3 * b.m_value.x,
            wOutA * a.m_outTan.y + wA * a.m_value.y + wInB * b.m_inTan.y + t3 * b.m_value.y,
            wOutA * a.m_outTan.z + wA * a.m_value.z + wInB * b.m_inTan.z + t3 * b.m_value.z,
        };
    }
    if (interpolation == 3) {
        // Hermite.
        float t2 = t * t;
        float t3 = t2 * t;
        float wInB = t3 - t2;
        float wB = t2 * kThree - (t3 + t3);
        float wOutA = (t3 - (t2 + t2)) + t;
        float wA = ((t3 + t3) - t2 * kThree) + 1.0f;
        return C3Vector{
            wOutA * a.m_outTan.x + wA * a.m_value.x + wB * b.m_value.x + wInB * b.m_inTan.x,
            wB * b.m_value.y + wInB * b.m_inTan.y + wA * a.m_value.y + wOutA * a.m_outTan.y,
            wB * b.m_value.z + wInB * b.m_inTan.z + wA * a.m_value.z + wOutA * a.m_outTan.z,
        };
    }
    return Lerp(a.m_value, b.m_value, t);
}

}  // namespace

// ref: FUN_1411fe990
void M2TrackInit(const M2Track<uint8_t>* track, M2TrackState<uint8_t>* state, const uint8_t* def) {
    state->m_value = *def;
    if (track->m_values.Count() != 0 && track->m_values[0].Count() != 0) {
        state->m_value = track->m_values[0][0];
    }
}

// ref: FUN_1411fe9d0
void M2TrackInit(const M2Track<uint16_t>* track, M2TrackState<uint16_t>* state, const uint16_t* def) {
    state->m_value = *def;
    if (track->m_values.Count() != 0 && track->m_values[0].Count() != 0) {
        state->m_value = track->m_values[0][0];
    }
}

// ref: FUN_1411fea10
void M2TrackInit(const M2Track<float>* track, M2TrackState<float>* state, const float* def) {
    state->m_value = *def;
    if (track->m_values.Count() != 0 && track->m_values[0].Count() != 0) {
        state->m_value = track->m_values[0][0];
    }
}

// ref: FUN_1411fea50
void M2TrackInit(const M2Track<M2CompQuat>* track, M2TrackState<C4Quaternion>* state, const C4Quaternion* def) {
    state->m_value = *def;
    if (track->m_values.Count() != 0 && track->m_values[0].Count() != 0) {
        state->m_value = UnpackQuat(track->m_values[0][0]);
    }
}

// ref: FUN_1411feb00
void M2TrackInit(const M2Track<C3Vector>* track, M2TrackState<C3Vector>* state, const C3Vector* def) {
    state->m_value = *def;
    if (track->m_values.Count() != 0 && track->m_values[0].Count() != 0) {
        state->m_value = track->m_values[0][0];
    }
}

// ref: FUN_1411feb50
void M2TrackInit(const M2Track<C4Quaternion>* track, M2TrackState<C4Quaternion>* state, const C4Quaternion* def) {
    state->m_value = *def;
    if (track->m_values.Count() != 0 && track->m_values[0].Count() != 0) {
        state->m_value = track->m_values[0][0];
    }
}

// ref: FUN_1411feb90
void M2TrackInitFixed16(const M2Track<uint16_t>* track, M2TrackState<float>* state, const float* def) {
    state->m_value = *def;
    if (track->m_values.Count() != 0 && track->m_values[0].Count() != 0) {
        state->m_value = static_cast<float>(static_cast<int16_t>(track->m_values[0][0])) * kFixed16Scale;
    }
}

// ref: FUN_1411fbee0
void M2TrackSample(const uint32_t* globalTimes, const M2AnimTarget* target,
    const M2Track<M2SplineKey<C3Vector>>* track, const M2Track<M2SplineKey<C3Vector>>* blendTrack,
    M2TrackState<C3Vector>* state, const C3Vector* def, uint32_t flags) {
    const M2SequenceTime* seq = target->m_sequence;
    uint32_t index = ValuesIndex(track, seq);
    uint16_t globalSequence = static_cast<uint16_t>(track->m_globalSequence);
    if (track->m_values[index].Count() == 0 || (flags & M2TRACK_SKIP_PRIMARY) != 0) {
        state->m_value = *def;
        if (track->m_interpolation == 0) {
            return;
        }
    } else {
        uint32_t k0;
        uint32_t k1;
        float t;
        SequenceKeys(globalTimes, track, seq, &state->m_key, k0, k1, t);
        const M2Array<M2SplineKey<C3Vector>>& values = track->m_values[index];
        if (track->m_interpolation == 0) {
            state->m_value = values[k0].m_value;
            return;
        }
        state->m_value = SplineValue(track->m_interpolation, values[k0], values[k1], t);
    }

    if (target->m_blendWeight == 0.0f || globalSequence != 0xffff) {
        return;
    }
    const M2SequenceTime* blendSeq = target->m_blendSequence;
    if (blendSeq == nullptr) {
        return;
    }
    C3Vector from;
    uint32_t blendIndex = blendSeq->m_sequence;
    if (blendTrack->m_values.Count() <= blendIndex) {
        blendIndex = 0;
    }
    if (blendTrack->m_values[blendIndex].Count() == 0 || (flags & M2TRACK_SKIP_BLEND) != 0) {
        from = *def;
    } else {
        uint32_t k0;
        uint32_t k1;
        float t;
        SequenceKeys(globalTimes, blendTrack, blendSeq, &state->m_blendKey, k0, k1, t);
        const M2Array<M2SplineKey<C3Vector>>& values = blendTrack->m_values[blendIndex];
        uint16_t interpolation = blendTrack->m_interpolation;
        if (interpolation == 2 || interpolation == 3) {
            from = SplineValue(interpolation, values[k0], values[k1], t);
        } else {
            from = Lerp(values[k0].m_value, values[k1].m_value, t);
        }
    }
    float w = target->m_blendWeight;
    state->m_value.x = (from.x - state->m_value.x) * w + state->m_value.x;
    state->m_value.y = (from.y - state->m_value.y) * w + state->m_value.y;
    state->m_value.z = (from.z - state->m_value.z) * w + state->m_value.z;
}

// ref: FUN_1411fc8e0
void M2TrackSample(const uint32_t* globalTimes, const M2AnimTarget* target, const M2Track<uint8_t>* track,
    const M2Track<uint8_t>* blendTrack, M2TrackState<uint8_t>* state, const uint8_t* def, uint32_t flags) {
    const M2SequenceTime* seq = target->m_sequence;
    uint32_t index = ValuesIndex(track, seq);
    uint16_t globalSequence = static_cast<uint16_t>(track->m_globalSequence);
    if (track->m_values.Count() == 0 || (flags & M2TRACK_SKIP_PRIMARY) != 0) {
        state->m_value = *def;
    } else {
        if (track->m_values[index].Count() == 0) {
            state->m_value = *def;
        } else {
            uint32_t k0;
            uint32_t k1;
            float t;
            SequenceKeys(globalTimes, track, seq, &state->m_key, k0, k1, t);
            state->m_value = track->m_values[index][k0];
        }
        if (track->m_interpolation == 0) {
            return;
        }
    }

    // The blended-from key follows along; a byte is not blended.
    if (target->m_blendWeight == 0.0f || globalSequence != 0xffff) {
        return;
    }
    const M2SequenceTime* blendSeq = target->m_blendSequence;
    if (blendSeq == nullptr || blendTrack->m_values.Count() == 0 || (flags & M2TRACK_SKIP_BLEND) != 0) {
        return;
    }
    uint32_t blendIndex = blendSeq->m_sequence;
    if (blendTrack->m_values.Count() <= blendIndex) {
        blendIndex = 0;
    }
    if (blendTrack->m_values[blendIndex].Count() == 0) {
        return;
    }
    uint32_t k0;
    uint32_t k1;
    float t;
    SequenceKeys(globalTimes, blendTrack, blendSeq, &state->m_blendKey, k0, k1, t);
}

// ref: FUN_1411fccc0
void M2TrackSample(const uint32_t* globalTimes, const M2AnimTarget* target, const M2Track<float>* track,
    const M2Track<float>* blendTrack, M2TrackState<float>* state, const float* def, uint32_t flags) {
    const M2SequenceTime* seq = target->m_sequence;
    uint32_t index = ValuesIndex(track, seq);
    uint16_t globalSequence = static_cast<uint16_t>(track->m_globalSequence);
    if (track->m_values.Count() == 0 || (flags & M2TRACK_SKIP_PRIMARY) != 0) {
        state->m_value = *def;
    } else if (track->m_values[index].Count() == 0) {
        state->m_value = *def;
        if (track->m_interpolation == 0) {
            return;
        }
    } else {
        uint32_t k0;
        uint32_t k1;
        float t;
        SequenceKeys(globalTimes, track, seq, &state->m_key, k0, k1, t);
        const M2Array<float>& values = track->m_values[index];
        if (track->m_interpolation == 0) {
            state->m_value = values[k0];
            return;
        }
        state->m_value = Lerp(values[k0], values[k1], t);
    }

    if (target->m_blendWeight == 0.0f || globalSequence != 0xffff) {
        return;
    }
    const M2SequenceTime* blendSeq = target->m_blendSequence;
    if (blendSeq == nullptr) {
        return;
    }
    float from = *def;
    if (blendTrack->m_values.Count() != 0 && (flags & M2TRACK_SKIP_BLEND) == 0) {
        uint32_t blendIndex = blendSeq->m_sequence;
        if (blendTrack->m_values.Count() <= blendIndex) {
            blendIndex = 0;
        }
        if (blendTrack->m_values[blendIndex].Count() != 0) {
            uint32_t k0;
            uint32_t k1;
            float t;
            SequenceKeys(globalTimes, blendTrack, blendSeq, &state->m_blendKey, k0, k1, t);
            const M2Array<float>& values = blendTrack->m_values[blendIndex];
            from = Lerp(values[k0], values[k1], t);
        }
    }
    state->m_value = (from - state->m_value) * target->m_blendWeight + state->m_value;
}

// ref: FUN_1411fd1d0
void M2TrackSample(const uint32_t* globalTimes, const M2AnimTarget* target, const M2Track<M2CompQuat>* track,
    const M2Track<M2CompQuat>* blendTrack, M2TrackState<C4Quaternion>* state, const C4Quaternion* def,
    uint32_t flags) {
    uint16_t globalSequence = static_cast<uint16_t>(track->m_globalSequence);
    if (track->m_values.Count() == 0 || (flags & M2TRACK_SKIP_PRIMARY) != 0) {
        state->m_value = *def;
    } else {
        const M2SequenceTime* seq = target->m_sequence;
        uint32_t index = ValuesIndex(track, seq);
        if (track->m_values[index].Count() != 0) {
            uint32_t k0;
            uint32_t k1;
            float t;
            SequenceKeys(globalTimes, track, seq, &state->m_key, k0, k1, t);
            const M2Array<M2CompQuat>& values = track->m_values[index];
            C4Quaternion a = UnpackQuat(values[k0]);
            if (track->m_interpolation == 0) {
                state->m_value = a;
                return;
            }
            C4Quaternion b = UnpackQuat(values[k1]);
            C4Quaternion out;
            state->m_value = *QuaternionNlerp(&out, t, &a, &b);
        } else {
            state->m_value = *def;
            if (track->m_interpolation == 0) {
                return;
            }
        }
    }

    if (target->m_blendWeight == 0.0f || globalSequence != 0xffff) {
        return;
    }
    const M2SequenceTime* blendSeq = target->m_blendSequence;
    if (blendSeq == nullptr) {
        return;
    }
    C4Quaternion from = *def;
    if (blendTrack->m_values.Count() != 0 && (flags & M2TRACK_SKIP_BLEND) == 0) {
        uint32_t blendIndex = blendSeq->m_sequence;
        if (blendTrack->m_values.Count() <= blendIndex) {
            blendIndex = 0;
        }
        if (blendTrack->m_values[blendIndex].Count() != 0) {
            uint32_t k0;
            uint32_t k1;
            float t;
            SequenceKeys(globalTimes, blendTrack, blendSeq, &state->m_blendKey, k0, k1, t);
            const M2Array<M2CompQuat>& values = blendTrack->m_values[blendIndex];
            C4Quaternion a = UnpackQuat(values[k0]);
            C4Quaternion b = UnpackQuat(values[k1]);
            QuaternionNlerp(&from, t, &a, &b);
        }
    }
    C4Quaternion current = state->m_value;
    C4Quaternion out;
    state->m_value = *QuaternionSlerp(&out, target->m_blendWeight, &current, &from);
}

// ref: FUN_1411fd910
void M2TrackSample(const uint32_t* globalTimes, const M2AnimTarget* target, const M2Track<C3Vector>* track,
    const M2Track<C3Vector>* blendTrack, M2TrackState<C3Vector>* state, const C3Vector* def, uint32_t flags) {
    const M2SequenceTime* seq = target->m_sequence;
    uint32_t index = ValuesIndex(track, seq);
    uint16_t globalSequence = static_cast<uint16_t>(track->m_globalSequence);
    if (track->m_values.Count() == 0 || (flags & M2TRACK_SKIP_PRIMARY) != 0) {
        state->m_value = *def;
    } else if (track->m_values[index].Count() == 0) {
        state->m_value = *def;
        if (track->m_interpolation == 0) {
            return;
        }
    } else {
        uint32_t k0;
        uint32_t k1;
        float t;
        SequenceKeys(globalTimes, track, seq, &state->m_key, k0, k1, t);
        const M2Array<C3Vector>& values = track->m_values[index];
        if (track->m_interpolation == 0) {
            state->m_value = values[k0];
            return;
        }
        state->m_value = Lerp(values[k0], values[k1], t);
    }

    if (target->m_blendWeight == 0.0f || globalSequence != 0xffff) {
        return;
    }
    const M2SequenceTime* blendSeq = target->m_blendSequence;
    if (blendSeq == nullptr) {
        return;
    }
    C3Vector from = *def;
    if (blendTrack->m_values.Count() != 0 && (flags & M2TRACK_SKIP_BLEND) == 0) {
        uint32_t blendIndex = blendSeq->m_sequence;
        if (blendTrack->m_values.Count() <= blendIndex) {
            blendIndex = 0;
        }
        if (blendTrack->m_values[blendIndex].Count() != 0) {
            uint32_t k0;
            uint32_t k1;
            float t;
            SequenceKeys(globalTimes, blendTrack, blendSeq, &state->m_blendKey, k0, k1, t);
            const M2Array<C3Vector>& values = blendTrack->m_values[blendIndex];
            from = Lerp(values[k0], values[k1], t);
        }
    }
    float w = target->m_blendWeight;
    state->m_value.x = (from.x - state->m_value.x) * w + state->m_value.x;
    state->m_value.y = (from.y - state->m_value.y) * w + state->m_value.y;
    state->m_value.z = (from.z - state->m_value.z) * w + state->m_value.z;
}

// ref: FUN_1411fdf00
void M2TrackSample(const uint32_t* globalTimes, const M2AnimTarget* target, const M2Track<C4Quaternion>* track,
    const M2Track<C4Quaternion>* blendTrack, M2TrackState<C4Quaternion>* state, const C4Quaternion* def,
    uint32_t flags) {
    const M2SequenceTime* seq = target->m_sequence;
    uint32_t index = ValuesIndex(track, seq);
    uint16_t globalSequence = static_cast<uint16_t>(track->m_globalSequence);
    if (track->m_values.Count() == 0 || (flags & M2TRACK_SKIP_PRIMARY) != 0) {
        state->m_value = *def;
    } else if (track->m_values[index].Count() == 0) {
        state->m_value = *def;
        if (track->m_interpolation == 0) {
            return;
        }
    } else {
        uint32_t k0;
        uint32_t k1;
        float t;
        SequenceKeys(globalTimes, track, seq, &state->m_key, k0, k1, t);
        const M2Array<C4Quaternion>& values = track->m_values[index];
        if (track->m_interpolation == 0) {
            state->m_value = values[k0];
            return;
        }
        C4Quaternion out;
        state->m_value = *QuaternionNlerp(&out, t, &values[k0], &values[k1]);
    }

    if (target->m_blendWeight == 0.0f || globalSequence != 0xffff) {
        return;
    }
    const M2SequenceTime* blendSeq = target->m_blendSequence;
    if (blendSeq == nullptr) {
        return;
    }
    C4Quaternion from = C4Quaternion{0.0f, 0.0f, 0.0f, 1.0f};
    if (blendTrack->m_values.Count() == 0 || (flags & M2TRACK_SKIP_BLEND) != 0) {
        from = *def;
    } else {
        uint32_t blendIndex = blendSeq->m_sequence;
        if (blendTrack->m_values.Count() <= blendIndex) {
            blendIndex = 0;
        }
        if (blendTrack->m_values[blendIndex].Count() == 0) {
            from = *def;
        } else {
            uint32_t k0;
            uint32_t k1;
            float t;
            SequenceKeys(globalTimes, blendTrack, blendSeq, &state->m_blendKey, k0, k1, t);
            const M2Array<C4Quaternion>& values = blendTrack->m_values[blendIndex];
            QuaternionNlerp(&from, t, &values[k0], &values[k1]);
        }
    }
    C4Quaternion current = state->m_value;
    C4Quaternion out;
    state->m_value = *QuaternionSlerp(&out, target->m_blendWeight, &current, &from);
}

// ref: FUN_1411fe450
void M2TrackSampleFixed16(const uint32_t* globalTimes, const M2AnimTarget* target, const M2Track<uint16_t>* track,
    const M2Track<uint16_t>* blendTrack, M2TrackState<float>* state, const float* def, uint32_t flags) {
    const M2SequenceTime* seq = target->m_sequence;
    uint32_t index = ValuesIndex(track, seq);
    uint16_t globalSequence = static_cast<uint16_t>(track->m_globalSequence);
    if (track->m_values.Count() == 0 || (flags & M2TRACK_SKIP_PRIMARY) != 0) {
        state->m_value = *def;
    } else if (track->m_values[index].Count() == 0) {
        state->m_value = *def;
        if (track->m_interpolation == 0) {
            return;
        }
    } else {
        uint32_t k0;
        uint32_t k1;
        float t;
        SequenceKeys(globalTimes, track, seq, &state->m_key, k0, k1, t);
        const M2Array<uint16_t>& values = track->m_values[index];
        float a = static_cast<float>(static_cast<int16_t>(values[k0])) * kFixed16Scale;
        if (track->m_interpolation == 0) {
            state->m_value = a;
            return;
        }
        float b = static_cast<float>(static_cast<int16_t>(values[k1])) * kFixed16Scale;
        state->m_value = (b - a) * t + a;
    }

    if (target->m_blendWeight == 0.0f || globalSequence != 0xffff) {
        return;
    }
    const M2SequenceTime* blendSeq = target->m_blendSequence;
    if (blendSeq == nullptr) {
        return;
    }
    float from = *def;
    if (blendTrack->m_values.Count() != 0 && (flags & M2TRACK_SKIP_BLEND) == 0) {
        uint32_t blendIndex = blendSeq->m_sequence;
        if (blendTrack->m_values.Count() <= blendIndex) {
            blendIndex = 0;
        }
        if (blendTrack->m_values[blendIndex].Count() != 0) {
            uint32_t k0;
            uint32_t k1;
            float t;
            SequenceKeys(globalTimes, blendTrack, blendSeq, &state->m_blendKey, k0, k1, t);
            const M2Array<uint16_t>& values = blendTrack->m_values[blendIndex];
            float a = static_cast<float>(static_cast<int16_t>(values[k0])) * kFixed16Scale;
            float b = static_cast<float>(static_cast<int16_t>(values[k1])) * kFixed16Scale;
            from = (b - a) * t + a;
        }
    }
    state->m_value = (from - state->m_value) * target->m_blendWeight + state->m_value;
}

// ref: FUN_141348580
C4Quaternion* QuaternionNlerp(C4Quaternion* out, float t, const C4Quaternion* a, const C4Quaternion* b) {
    float x = (b->x - a->x) * t + a->x;
    float y = (b->y - a->y) * t + a->y;
    float w = (b->w - a->w) * t + a->w;
    float z = (b->z - a->z) * t + a->z;
    float lengthSq = y * y + x * x + z * z + w * w;
    // Newton steps on 1 / sqrt from a linear start (DAT_14273492c..DAT_14273493c).
    float inv = 1.0214350f - (lengthSq - 0.9590657f) * 0.5325160f;
    if (lengthSq <= 0.9152210f) {
        inv = inv * (1.0214350f - (inv * inv * lengthSq - 0.9590657f) * 0.5325160f);
        if (lengthSq <= 0.6521196f) {
            inv = inv * (1.0214350f - (inv * inv * lengthSq - 0.9590657f) * 0.5325160f);
        }
    }
    out->y = y * inv;
    out->z = z * inv;
    out->x = x * inv;
    out->w = w * inv;
    return out;
}

// ref: FUN_1413486d0
C4Quaternion* QuaternionSlerp(C4Quaternion* out, float t, const C4Quaternion* a, const C4Quaternion* b) {
    float cosTheta = b->w * a->w + b->z * a->z + b->x * a->x + b->y * a->y;
    float sign = 1.0f;
    if (cosTheta < 0.0f) {
        cosTheta = -cosTheta;
        sign = -1.0f;
    }
    float sinTheta = std::sqrt(std::fabs(1.0f - cosTheta * cosTheta));
    // DAT_14263241c
    if (4.76837158e-07f <= std::fabs(sinTheta)) {
        float theta = std::atan2(sinTheta, cosTheta);
        float invSin = 1.0f / sinTheta;
        float wa = std::sin((1.0f - t) * theta) * invSin;
        float wb = std::sin(theta * t) * invSin * sign;
        out->x = b->x * wb + a->x * wa;
        out->y = b->y * wb + a->y * wa;
        out->z = b->z * wb + a->z * wa;
        out->w = b->w * wb + a->w * wa;
    } else {
        *out = *a;
    }
    return out;
}
