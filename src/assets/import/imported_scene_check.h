#pragma once
// ── imported_scene_check — what every ImportedScene must satisfy ──────────────
//
// The STRUCTURAL half of the import-frontend contract: invariants that hold for
// any valid scene, whatever file or library it came from. The contract suite
// (tests/import_contract.h) runs this on every front end's output; the back end
// may run it before trusting a scene. The SEMANTIC half (does this file mean
// that geometry?) needs a known source, and lives in the suite.
//
// Each violation names the check that failed and the element, so a front end
// author sees "weights: mesh 'Body' vertex 12 sums to 0.97" rather than a bool.
#include "assets/import/imported_scene.h"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace imp {

struct Violation {
    const char* check;                   // "streams", "indices", "weights", …
    std::string detail;
};

// ── Small math, only what the checks and the contract suite need ────────────
inline Float4x4 mul(const Float4x4& a, const Float4x4& b) {        // a * b
    Float4x4 r;
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}
inline Float3 transformPoint(const Float4x4& t, const Float3& p) {
    return {t.m[0] * p.x + t.m[4] * p.y + t.m[8]  * p.z + t.m[12],
            t.m[1] * p.x + t.m[5] * p.y + t.m[9]  * p.z + t.m[13],
            t.m[2] * p.x + t.m[6] * p.y + t.m[10] * p.z + t.m[14]};
}
inline bool nearIdentity(const Float4x4& t, float eps) {
    const Float4x4 id;
    for (int i = 0; i < 16; ++i)
        if (std::fabs(t.m[i] - id.m[i]) > eps) return false;
    return true;
}
// General 4x4 inverse (cofactors). A singular matrix returns all zeros.
inline Float4x4 inverse(const Float4x4& a) {
    const float* m = a.m; float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    const float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    Float4x4 r;
    for (int i = 0; i < 16; ++i) r.m[i] = det != 0 ? inv[i] / det : 0.0f;
    return r;
}
// World matrix of element `i` in a parents-before-children array.
template <class T>
Float4x4 worldOf(const std::vector<T>& items, size_t i, Float4x4 T::*local) {
    Float4x4 w = items[i].*local;
    for (int32_t p = items[i].parent; p >= 0; p = items[(size_t)p].parent)
        w = mul(items[(size_t)p].*local, w);
    return w;
}

namespace detail {
inline bool finite3(const Float3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline bool finite4(const Float4& v) { return finite3({v.x, v.y, v.z}) && std::isfinite(v.w); }
inline std::string fmt(const char* f, auto... a) {
    char buf[256]; std::snprintf(buf, sizeof buf, f, a...); return buf;
}
}  // namespace detail

inline std::vector<Violation> checkScene(const ImportedScene& s) {
    using detail::fmt;
    std::vector<Violation> v;
    auto bad = [&](const char* check, std::string d) { v.push_back({check, std::move(d)}); };

    // ── empty: an empty scene is ImportError::Empty, never a result ─────────
    size_t triangles = 0;
    for (const auto& m : s.meshes) triangles += m.indices.size() / 3;
    if (triangles == 0 && s.clips.empty())
        bad("empty", "no triangles and no clips: a front end must return ImportError::Empty");

    const size_t boneCount = s.skeleton ? s.skeleton->bones.size() : 0;

    // ── meshes ──────────────────────────────────────────────────────────────
    for (size_t mi = 0; mi < s.meshes.size(); ++mi) {
        const Mesh& m = s.meshes[mi];
        const std::string who = fmt("mesh %zu '%s'", mi, m.name.c_str());
        const size_t n = m.positions.size();

        if (m.normals.size() != n)
            bad("streams", fmt("%s: %zu normals for %zu positions", who.c_str(), m.normals.size(), n));
        for (auto [name, size] : {std::pair{"tangents", m.tangents.size()}, {"uv0", m.uv0.size()},
                                  {"joints", m.joints.size()}, {"weights", m.weights.size()}})
            if (size != 0 && size != n)
                bad("streams", fmt("%s: %zu %s for %zu positions (a stream is empty or complete)",
                                   who.c_str(), size, name, n));
        if (m.joints.empty() != m.weights.empty())
            bad("streams", who + ": joints and weights must be both present or both empty");

        for (size_t i = 0; i < n; ++i)
            if (!detail::finite3(m.positions[i]) || (i < m.normals.size() && !detail::finite3(m.normals[i]))) {
                bad("finite", fmt("%s vertex %zu has a non-finite position or normal", who.c_str(), i));
                break;
            }
        for (size_t i = 0; i < m.tangents.size(); ++i)
            if (!detail::finite4(m.tangents[i]) || std::fabs(std::fabs(m.tangents[i].w) - 1.0f) > 1e-3f) {
                bad("tangents", fmt("%s vertex %zu: tangent w must be +1 or -1", who.c_str(), i));
                break;
            }

        if (m.indices.empty())
            bad("indices", who + ": no indices (a mesh with no triangles should not be in the scene)");
        if (m.indices.size() % 3)
            bad("indices", fmt("%s: %zu indices is not a multiple of 3", who.c_str(), m.indices.size()));
        for (size_t i = 0; i < m.indices.size(); ++i)
            if (m.indices[i] >= n) {
                bad("indices", fmt("%s: index %zu = %u, but there are %zu vertices",
                                   who.c_str(), i, m.indices[i], n));
                break;
            }

        // Submeshes tile [0, indices.size()) in order: no gap, no overlap.
        if (m.submeshes.empty() && !m.indices.empty())
            bad("submeshes", who + ": no submeshes");
        uint32_t next = 0;
        for (size_t si = 0; si < m.submeshes.size(); ++si) {
            const Submesh& sm = m.submeshes[si];
            if (sm.firstIndex != next)
                bad("submeshes", fmt("%s submesh %zu starts at %u, expected %u (gap or overlap)",
                                     who.c_str(), si, sm.firstIndex, next));
            if (sm.indexCount == 0 || sm.indexCount % 3)
                bad("submeshes", fmt("%s submesh %zu has %u indices (positive multiple of 3 required)",
                                     who.c_str(), si, sm.indexCount));
            if (sm.material >= s.materials.size())
                bad("submeshes", fmt("%s submesh %zu uses material %u of %zu",
                                     who.c_str(), si, sm.material, s.materials.size()));
            next = sm.firstIndex + sm.indexCount;
        }
        if (!m.submeshes.empty() && next != m.indices.size())
            bad("submeshes", fmt("%s: submeshes end at %u, indices at %zu", who.c_str(), next, m.indices.size()));

        // Skinning: joints in range, weights non-negative and normalised.
        if (m.skinned() && boneCount == 0)
            bad("weights", who + ": skinned, but the scene has no skeleton");
        for (size_t i = 0; i < m.weights.size() && boneCount; ++i) {
            const Float4& w = m.weights[i];
            const auto&   j = m.joints[i];
            const float sum = w.x + w.y + w.z + w.w;
            if (w.x < 0 || w.y < 0 || w.z < 0 || w.w < 0 || std::fabs(sum - 1.0f) > 1e-3f) {
                bad("weights", fmt("%s vertex %zu: weights (%g %g %g %g) sum to %g, must be >=0 and sum to 1",
                                   who.c_str(), i, w.x, w.y, w.z, w.w, sum));
                break;
            }
            const float ws[4] = {w.x, w.y, w.z, w.w};
            bool jointBad = false;
            for (int k = 0; k < 4; ++k)
                if (ws[k] > 0 && j[k] >= boneCount) jointBad = true;
            if (jointBad) {
                bad("weights", fmt("%s vertex %zu names a joint past the %zu bones", who.c_str(), i, boneCount));
                break;
            }
        }
    }

    // ── nodes ───────────────────────────────────────────────────────────────
    if (!s.meshes.empty() && s.nodes.empty())
        bad("nodes", "meshes but no nodes: nothing places them");
    std::vector<bool> placed(s.meshes.size(), false);
    for (size_t ni = 0; ni < s.nodes.size(); ++ni) {
        const Node& nd = s.nodes[ni];
        if (ni == 0 ? nd.parent != -1 : (nd.parent < 0 || (size_t)nd.parent >= ni))
            bad("nodes", fmt("node %zu '%s' has parent %d (node 0 is the root; others need an "
                             "earlier parent)", ni, nd.name.c_str(), nd.parent));
        for (uint32_t mi : nd.meshes) {
            if (mi >= s.meshes.size())
                bad("nodes", fmt("node %zu '%s' references mesh %u of %zu", ni, nd.name.c_str(), mi, s.meshes.size()));
            else
                placed[mi] = true;
        }
    }
    for (size_t mi = 0; mi < placed.size(); ++mi)
        if (!placed[mi])
            bad("nodes", fmt("mesh %zu '%s' is on no node, so it would never be cooked",
                             mi, s.meshes[mi].name.c_str()));

    // ── materials ───────────────────────────────────────────────────────────
    for (size_t i = 0; i < s.materials.size(); ++i)
        for (const TextureRef* t : {&s.materials[i].baseColor, &s.materials[i].normal}) {
            if ((!t->path.empty()) + (!t->embedded.empty()) + (!t->rgba.empty()) > 1)
                bad("textures", fmt("material %zu '%s': a texture is a path, embedded bytes OR pixels, not two",
                                    i, s.materials[i].name.c_str()));
            if (!t->rgba.empty() && (size_t)t->width * t->height * 4 != t->rgba.size())
                bad("textures", fmt("material %zu '%s': rgba holds %zu bytes, not %ux%ux4",
                                    i, s.materials[i].name.c_str(), t->rgba.size(), t->width, t->height));
            if ((!t->embedded.empty() || !t->rgba.empty()) && t->embeddedName.empty())
                bad("textures", fmt("material %zu '%s': embedded texture with no name",
                                    i, s.materials[i].name.c_str()));
        }

    // ── skeleton ────────────────────────────────────────────────────────────
    std::set<std::string> boneNames;
    if (s.skeleton) {
        const auto& bones = s.skeleton->bones;
        if (bones.empty()) bad("skeleton", "a skeleton with no bones (use no skeleton)");
        for (size_t bi = 0; bi < bones.size(); ++bi) {
            const Bone& b = bones[bi];
            if (b.parent >= (int32_t)bi || b.parent < -1)
                bad("skeleton", fmt("bone %zu '%s' has parent %d (parents come first)", bi, b.name.c_str(), b.parent));
            if (!boneNames.insert(b.name).second)
                bad("skeleton", fmt("bone name '%s' is used twice (clips bind by name)", b.name.c_str()));
        }
        // inverseBind must be finite and invertible. It need NOT be the inverse
        // of the rest pose: real files bind skins in other poses (Bone's comment).
        for (size_t bi = 0; bi < bones.size(); ++bi) {
            const float* m = bones[bi].inverseBind.m;
            bool finite = true;
            for (int k = 0; k < 16; ++k) finite &= std::isfinite(m[k]);
            const float det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2])
                            + m[8] * (m[1] * m[6] - m[5] * m[2]);
            const float c0 = std::sqrt(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]), c1 = std::sqrt(m[4]*m[4] + m[5]*m[5] + m[6]*m[6]),
                        c2 = std::sqrt(m[8]*m[8] + m[9]*m[9] + m[10]*m[10]);
            if (!finite || !(std::fabs(det) > 1e-6f * c0 * c1 * c2)) {   // scale-invariant, like normalMatrix
                bad("skeleton", fmt("bone %zu '%s': inverseBind is not finite and invertible",
                                    bi, bones[bi].name.c_str()));
                break;
            }
        }
    }

    // ── clips ───────────────────────────────────────────────────────────────
    std::set<std::string> clipNames;
    for (const Clip& c : s.clips) {
        if (c.name.empty() || !clipNames.insert(c.name).second)
            bad("clips", "clip names must be non-empty and unique: '" + c.name + "'");
        if (!std::isfinite(c.duration) || c.duration < 0)
            bad("clips", fmt("clip '%s' has duration %g", c.name.c_str(), c.duration));
        if (!s.skeleton)
            bad("clips", "clip '" + c.name + "' but no skeleton: node animation is a Dropped entry (Animation)");
        for (const Track& t : c.tracks) {
            if (s.skeleton && !boneNames.count(t.bone))
                bad("clips", fmt("clip '%s' animates '%s', which is not a bone", c.name.c_str(), t.bone.c_str()));
            auto keysOk = [&](const auto& keys, const char* ch) {
                float prev = -1;
                for (const auto& k : keys) {
                    if (!std::isfinite(k.time) || k.time < prev || k.time < 0 || k.time > c.duration + 1e-4f) {
                        bad("clips", fmt("clip '%s' bone '%s' %s: key time %g out of order or outside [0, %g]",
                                         c.name.c_str(), t.bone.c_str(), ch, k.time, c.duration));
                        return;
                    }
                    prev = k.time;
                }
            };
            keysOk(t.translation, "translation"); keysOk(t.rotation, "rotation"); keysOk(t.scale, "scale");
            // Values: finite, and every rotation a unit quaternion (front ends
            // normalise; one that could not be normalised is not a rotation).
            auto valuesOk = [&](const std::vector<KeyF3>& keys, const char* ch) {
                for (const auto& k : keys)
                    if (!detail::finite3(k.value)) {
                        bad("clips", fmt("clip '%s' bone '%s' %s: a key at t=%g is not finite",
                                         c.name.c_str(), t.bone.c_str(), ch, k.time));
                        return;
                    }
            };
            valuesOk(t.translation, "translation"); valuesOk(t.scale, "scale");
            for (const auto& k : t.rotation) {
                const Quat& q = k.value;
                const float n2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
                if (!std::isfinite(n2) || std::fabs(n2 - 1.0f) > 2e-3f) {
                    bad("clips", fmt("clip '%s' bone '%s' rotation: the key at t=%g is not a unit quaternion (|q|^2 = %g)",
                                     c.name.c_str(), t.bone.c_str(), k.time, n2));
                    break;
                }
            }
        }
    }

    // ── dropped: every entry says what was lost ─────────────────────────────
    for (const Dropped& d : s.dropped)
        if (d.what.empty() || d.count == 0)
            bad("dropped", std::string("a dropped '") + toString(d.kind) + "' entry must say what, and count >= 1");

    return v;
}

}  // namespace imp
