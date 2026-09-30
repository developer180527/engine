#include "assets/anim_from_scene.h"
#include "animation/ozz_bridge.h"   // finishOzzClip

#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/runtime/skeleton.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace imp {

namespace {

float dot(Float3 a, Float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 scale(Float3 v, float s) { return {v.x * s, v.y * s, v.z * s}; }
float det3(const Float4x4& t) {
    const float* m = t.m;
    return m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2])
         + m[8] * (m[1] * m[6] - m[5] * m[2]);
}

// Translation, rotation (the source's column-vector convention) and scale of a
// column-major matrix. A mirrored basis (negative determinant) puts the sign on
// the x scale, so the rotation stays proper.
void decompose(const Float4x4& t, Float3& pos, Quat& rot, Float3& scl) {
    const float* m = t.m;
    pos = {m[12], m[13], m[14]};
    Float3 c0{m[0], m[1], m[2]}, c1{m[4], m[5], m[6]}, c2{m[8], m[9], m[10]};
    scl = {std::sqrt(dot(c0, c0)), std::sqrt(dot(c1, c1)), std::sqrt(dot(c2, c2))};
    if (det3(t) < 0) scl.x = -scl.x;
    c0 = scale(c0, scl.x != 0 ? 1.0f / scl.x : 0); c1 = scale(c1, scl.y != 0 ? 1.0f / scl.y : 0);
    c2 = scale(c2, scl.z != 0 ? 1.0f / scl.z : 0);
    // Rotation matrix R (columns c0 c1 c2) -> quaternion (Shepperd).
    const float r00 = c0.x, r11 = c1.y, r22 = c2.z, tr = r00 + r11 + r22;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2;
        rot = {(c1.z - c2.y) / s, (c2.x - c0.z) / s, (c0.y - c1.x) / s, 0.25f * s};
    } else if (r00 > r11 && r00 > r22) {
        const float s = std::sqrt(1.0f + r00 - r11 - r22) * 2;
        rot = {0.25f * s, (c1.x + c0.y) / s, (c2.x + c0.z) / s, (c1.z - c2.y) / s};
    } else if (r11 > r22) {
        const float s = std::sqrt(1.0f + r11 - r00 - r22) * 2;
        rot = {(c1.x + c0.y) / s, 0.25f * s, (c2.y + c1.z) / s, (c2.x - c0.z) / s};
    } else {
        const float s = std::sqrt(1.0f + r22 - r00 - r11) * 2;
        rot = {(c2.x + c0.z) / s, (c2.y + c1.z) / s, 0.25f * s, (c0.y - c1.x) / s};
    }
}

}  // namespace

::Skeleton toAnimSkeleton(const Skeleton& in) {
    ::Skeleton out;
    out.bones.reserve(in.bones.size());
    for (const Bone& b : in.bones) {
        ::Bone ab;
        ab.name = b.name;
        ab.parentIndex = b.parent;
        Float3 p, s; Quat q;
        decompose(b.bindLocal, p, q, s);
        ab.bindPosition = {p.x, p.y, p.z};
        ab.bindRotation = {-q.x, -q.y, -q.z, q.w};
        ab.bindScale    = {s.x, s.y, s.z};
        std::memcpy(ab.localBindMatrix,   b.bindLocal.m,   sizeof ab.localBindMatrix);
        std::memcpy(ab.inverseBindMatrix, b.inverseBind.m, sizeof ab.inverseBindMatrix);
        out.bones.push_back(ab);
    }
    out.buildBoneMap();
    return out;
}

::AnimClip buildOzzClip(const Clip& c, const ::Skeleton& skel) {
    if (!skel.ozz) { LOG_ERROR("Anim", "buildOzzClip: skeleton has no ozz data"); return {}; }
    ozz::animation::offline::RawAnimation raw;
    raw.duration = std::max(c.duration, 1e-4f);
    raw.tracks.resize((size_t)skel.ozz->num_joints());
    int mapped = 0;
    auto t = [&](float time) { return std::clamp(time, 0.0f, raw.duration); };
    for (const Track& tr : c.tracks) {
        const int ours = skel.findBone(tr.bone);
        if (ours < 0) continue;
        auto& track = raw.tracks[(size_t)skel.ozzJointOf[ours]];
        for (const auto& k : tr.translation) track.translations.push_back({t(k.time), {k.value.x, k.value.y, k.value.z}});
        for (const auto& k : tr.rotation)    track.rotations.push_back({t(k.time), {k.value.x, k.value.y, k.value.z, k.value.w}});
        for (const auto& k : tr.scale)       track.scales.push_back({t(k.time), {k.value.x, k.value.y, k.value.z}});
        ++mapped;
    }
    return anim::finishOzzClip(raw, skel, c.name, mapped, (int)c.tracks.size());
}

}  // namespace imp
