#pragma once

#include <bx/math.h>
#include <cmath>
#include <vector>

#include "animation/animation_clip.h"

namespace anim {

// ── Vector interpolation ────────────────────────────────────────────────────

inline bx::Vec3 lerpVec3(const bx::Vec3& a, const bx::Vec3& b, float t) {
    return {
        a.x + (b.x - a.x) * t,
        a.y + (b.y - a.y) * t,
        a.z + (b.z - a.z) * t,
    };
}

// ── Keyframe search ─────────────────────────────────────────────────────────
// Returns the index of the LAST keyframe at or before `time`.
// If time is before the first key, returns 0.
// If time is at or past the last key, returns keyCount - 1.

inline int findKeyIndex(const std::vector<float>& timestamps, float time) {
    if (timestamps.empty()) return 0;
    if (time <= timestamps.front()) return 0;
    if (time >= timestamps.back()) return (int)timestamps.size() - 1;

    // Binary search for the interval containing time
    int lo = 0, hi = (int)timestamps.size() - 1;
    while (lo < hi - 1) {
        int mid = (lo + hi) / 2;
        if (timestamps[mid] <= time) lo = mid;
        else hi = mid;
    }
    return lo;
}

// ── Channel sampling ────────────────────────────────────────────────────────
// Evaluates a single channel at `time`, writes result into `out`.
// out must point to componentCount() floats (3 for vec3, 4 for quat).

inline void sampleChannel(const AnimChannel& ch, float time, float* out) {
    const int cc = ch.componentCount();
    const int vpk = ch.valuesPerKey();

    if (ch.keyCount() == 0) {
        for (int i = 0; i < cc; ++i) out[i] = 0.0f;
        return;
    }
    if (ch.keyCount() == 1) {
        int valueOffset = (ch.interpolation == AnimInterp::CubicSpline) ? cc : 0;
        for (int i = 0; i < cc; ++i)
            out[i] = ch.values[valueOffset + i];
        return;
    }

    int k0 = findKeyIndex(ch.timestamps, time);
    int k1 = k0 + 1;

    // Clamp: at or past end, return last key value
    if (k1 >= ch.keyCount()) {
        int off = k0 * vpk;
        int valueOffset = (ch.interpolation == AnimInterp::CubicSpline) ? cc : 0;
        for (int i = 0; i < cc; ++i)
            out[i] = ch.values[off + valueOffset + i];
        return;
    }

    float t0 = ch.timestamps[k0];
    float t1 = ch.timestamps[k1];
    float alpha = (t1 > t0) ? (time - t0) / (t1 - t0) : 0.0f;

    switch (ch.interpolation) {
    case AnimInterp::Step: {
        int off = k0 * vpk;
        for (int i = 0; i < cc; ++i) out[i] = ch.values[off + i];
        break;
    }
    case AnimInterp::Linear: {
        int off0 = k0 * cc;
        int off1 = k1 * cc;
        if (ch.property == AnimProperty::Rotation) {
            bx::Quaternion qa { ch.values[off0], ch.values[off0+1], ch.values[off0+2], ch.values[off0+3] };
            bx::Quaternion qb { ch.values[off1], ch.values[off1+1], ch.values[off1+2], ch.values[off1+3] };
            bx::Quaternion r = bx::slerp(qa, qb, alpha);
            out[0] = r.x; out[1] = r.y; out[2] = r.z; out[3] = r.w;
        } else {
            for (int i = 0; i < cc; ++i)
                out[i] = ch.values[off0 + i] + (ch.values[off1 + i] - ch.values[off0 + i]) * alpha;
        }
        break;
    }
    case AnimInterp::CubicSpline: {
        // Cubic hermite: p(t) = (2t^3 - 3t^2 + 1)*p0 + (t^3 - 2t^2 + t)*m0
        //              + (-2t^3 + 3t^2)*p1 + (t^3 - t^2)*m1
        float dt = t1 - t0;
        float t2 = alpha * alpha;
        float t3 = t2 * alpha;
        float h00 =  2*t3 - 3*t2 + 1;
        float h10 =    t3 - 2*t2 + alpha;
        float h01 = -2*t3 + 3*t2;
        float h11 =    t3 -   t2;

        // Layout per key: [in-tangent(cc), value(cc), out-tangent(cc)]
        int off0 = k0 * vpk;
        int off1 = k1 * vpk;
        const float* v0  = &ch.values[off0 + cc];      // value at k0
        const float* b0  = &ch.values[off0 + 2 * cc];  // out-tangent at k0
        const float* v1  = &ch.values[off1 + cc];      // value at k1
        const float* a1  = &ch.values[off1];            // in-tangent at k1

        for (int i = 0; i < cc; ++i)
            out[i] = h00 * v0[i] + h10 * dt * b0[i] + h01 * v1[i] + h11 * dt * a1[i];

        if (ch.property == AnimProperty::Rotation) {
            bx::Quaternion r { out[0], out[1], out[2], out[3] };
            r = bx::normalize(r);
            out[0] = r.x; out[1] = r.y; out[2] = r.z; out[3] = r.w;
        }
        break;
    }
    }
}

} // namespace anim
