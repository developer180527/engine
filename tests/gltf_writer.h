#pragma once
// ── gltf_writer — ImportedScene -> a real .gltf file, for tests ──────────────
// JSON with one embedded base64 buffer. Moved out of frontend_cgltf_test so the
// import fuzz target (fuzz_import_frontend_test) writes the same files the
// contract suite reads: its generated inputs start from the reference cases.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "assets/import/imported_scene.h"
#include "assets/import/imported_scene_check.h"   // mul, inverse, transformPoint

// ── A minimal glTF writer, for tests ────────────────────────────────────────
namespace gltfw {
using namespace imp;
// The engine's own types share names with the import format's (Skeleton and
// Bone in animation, Mesh and Material in the renderer). In a test that
// includes both (clip_cook_test, async_loader_test), these mean the import
// format's: a using-declaration outranks the using-directive above.
using imp::Bone;
using imp::Clip;
using imp::Material;
using imp::Mesh;
using imp::Node;
using imp::Skeleton;
using imp::Submesh;
using imp::Track;

struct Buffer {
    std::vector<uint8_t> data;
    std::string views, accessors;
    int nViews = 0, nAccessors = 0;

    // Appends floats as one bufferView + accessor; returns the accessor index.
    int floats(const float* f, size_t count, int comps, const char* type, bool minmax = false) {
        while (data.size() % 4) data.push_back(0);
        const size_t off = data.size();
        data.insert(data.end(), (const uint8_t*)f, (const uint8_t*)(f + count * comps));
        views += (nViews ? "," : "") + std::string("{\"buffer\":0,\"byteOffset\":") + std::to_string(off) +
                 ",\"byteLength\":" + std::to_string(count * comps * 4) + "}";
        std::string mm;
        if (minmax) {
            std::string lo = ",\"min\":[", hi = "],\"max\":[";
            for (int k = 0; k < comps; ++k) {
                float l = 1e30f, h = -1e30f;
                for (size_t i = 0; i < count; ++i) { l = std::min(l, f[i * comps + k]); h = std::max(h, f[i * comps + k]); }
                char v[64]; std::snprintf(v, sizeof v, "%s%g", k ? "," : "", l); lo += v;
                std::snprintf(v, sizeof v, "%s%g", k ? "," : "", h); hi += v;
            }
            mm = lo + hi + "]";
        }
        accessors += (nAccessors ? "," : "") + std::string("{\"bufferView\":") + std::to_string(nViews) +
                     ",\"componentType\":5126,\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"" + mm + "}";
        ++nViews;
        return nAccessors++;
    }
    int u16(const uint16_t* v, size_t count, int comps, const char* type) {
        while (data.size() % 4) data.push_back(0);
        const size_t off = data.size();
        data.insert(data.end(), (const uint8_t*)v, (const uint8_t*)(v + count * comps));
        views += (nViews ? "," : "") + std::string("{\"buffer\":0,\"byteOffset\":") + std::to_string(off) +
                 ",\"byteLength\":" + std::to_string(count * comps * 2) + "}";
        accessors += (nAccessors ? "," : "") + std::string("{\"bufferView\":") + std::to_string(nViews) +
                     ",\"componentType\":5123,\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"}";
        ++nViews;
        return nAccessors++;
    }
    int indices(const uint32_t* ix, size_t count) {
        while (data.size() % 4) data.push_back(0);
        const size_t off = data.size();
        data.insert(data.end(), (const uint8_t*)ix, (const uint8_t*)(ix + count));
        views += (nViews ? "," : "") + std::string("{\"buffer\":0,\"byteOffset\":") + std::to_string(off) +
                 ",\"byteLength\":" + std::to_string(count * 4) + "}";
        accessors += (nAccessors ? "," : "") + std::string("{\"bufferView\":") + std::to_string(nViews) +
                     ",\"componentType\":5125,\"count\":" + std::to_string(count) + ",\"type\":\"SCALAR\"}";
        ++nViews;
        return nAccessors++;
    }
};

std::string base64(const std::vector<uint8_t>& in) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < in.size() ? (uint32_t)in[i + 1] << 8 : 0) |
                           (i + 2 < in.size() ? in[i + 2] : 0);
        out += t[(v >> 18) & 63]; out += t[(v >> 12) & 63];
        out += i + 1 < in.size() ? t[(v >> 6) & 63] : '=';
        out += i + 2 < in.size() ? t[v & 63] : '=';
    }
    return out;
}

struct Extras { bool morph = false, colours = false, camera = false, spline = false; };

// A file authored in other units/axes. glTF declares neither, so an exporter
// puts the conversion in a root node: C = scale by `unit`, then Z-up -> Y-up
// ((x, y, z) -> (x, z, -y)). Everything else in the file is written in the
// authored frame: C^-1 * M * C for a transform, C^-1 * p for a point.
struct Authoring { float unit = 1.0f; bool zUp = false; };

struct Frame {
    Float4x4 c, ci; bool zUp;
    explicit Frame(const Authoring& a) : zUp(a.zUp) {
        if (a.zUp) { c.m[5] = 0; c.m[6] = -1; c.m[9] = 1; c.m[10] = 0; }
        for (int i : {0, 1, 2, 4, 5, 6, 8, 9, 10}) c.m[i] *= a.unit;
        ci = inverse(c);
    }
    Float4x4 transform(const Float4x4& m) const { return mul(ci, mul(m, c)); }
    Float3 point(Float3 p) const { return transformPoint(ci, p); }
    Float3 dir(Float3 v) const {                              // rotation only; normalised
        Float3 r = zUp ? Float3{v.x, -v.z, v.y} : v;
        const float l = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
        return {r.x / l, r.y / l, r.z / l};
    }
    Quat rotation(Quat q) const {                             // the same rotation, about the authored axes
        const Float3 a = zUp ? Float3{q.x, -q.z, q.y} : Float3{q.x, q.y, q.z};
        return {a.x, a.y, a.z, q.w};
    }
};

// An ImportedScene as glTF: one glTF mesh per imp::Mesh, one primitive per
// submesh (sharing the vertex accessors). A skinned mesh hangs from its own
// node carrying the skin, whose transform glTF ignores. Bones are nodes with
// TRS (glTF forbids a matrix on an animated node), clips are animations.
std::string write(const ImportedScene& s, Extras x = {}, Authoring au = {}) {
    const Frame f(au);
    Buffer b;
    std::string meshes, materials, nodes, skins, animations;
    const size_t N = s.nodes.size();
    std::vector<size_t> skinnedMeshes;
    for (size_t mi = 0; mi < s.meshes.size(); ++mi) if (s.meshes[mi].skinned()) skinnedMeshes.push_back(mi);
    const size_t firstBone = N + skinnedMeshes.size();
    const size_t boneCount = s.skeleton ? s.skeleton->bones.size() : 0;

    for (size_t mi = 0; mi < s.meshes.size(); ++mi) {
        const Mesh& m = s.meshes[mi];
        std::vector<float> P, Nn;
        for (size_t v = 0; v < m.positions.size(); ++v) {
            const Float3 p = f.point(m.positions[v]), n = f.dir(m.normals[v]);
            P.insert(P.end(), {p.x, p.y, p.z}); Nn.insert(Nn.end(), {n.x, n.y, n.z});
        }
        const int pos = b.floats(P.data(), m.positions.size(), 3, "VEC3", true);
        const int nrm = b.floats(Nn.data(), m.normals.size(), 3, "VEC3");
        const int uv  = m.uv0.empty() ? -1 : b.floats(&m.uv0[0].x, m.uv0.size(), 2, "VEC2");
        int col = -1, morph = -1, joints = -1, weights = -1;
        if (x.colours) { std::vector<float> c(m.positions.size() * 4, 1.0f); col = b.floats(c.data(), m.positions.size(), 4, "VEC4"); }
        if (x.morph)   { std::vector<float> d(m.positions.size() * 3, 0.0f); morph = b.floats(d.data(), m.positions.size(), 3, "VEC3", true); }
        if (m.skinned()) {
            std::vector<uint16_t> j; for (const auto& q : m.joints) j.insert(j.end(), q.begin(), q.end());
            joints  = b.u16(j.data(), m.joints.size(), 4, "VEC4");
            weights = b.floats(&m.weights[0].x, m.weights.size(), 4, "VEC4");
        }
        std::string prims;
        for (size_t si = 0; si < m.submeshes.size(); ++si) {
            const Submesh& sm = m.submeshes[si];
            const int ix = b.indices(&m.indices[sm.firstIndex], sm.indexCount);
            prims += (si ? "," : "") + std::string("{\"attributes\":{\"POSITION\":") + std::to_string(pos) +
                     ",\"NORMAL\":" + std::to_string(nrm) + (uv >= 0 ? ",\"TEXCOORD_0\":" + std::to_string(uv) : "") +
                     (col >= 0 ? ",\"COLOR_0\":" + std::to_string(col) : "") +
                     (joints >= 0 ? ",\"JOINTS_0\":" + std::to_string(joints) + ",\"WEIGHTS_0\":" + std::to_string(weights) : "") +
                     "},\"indices\":" + std::to_string(ix) + ",\"material\":" + std::to_string(sm.material) +
                     (morph >= 0 ? ",\"targets\":[{\"POSITION\":" + std::to_string(morph) + "},{\"POSITION\":" + std::to_string(morph) + "}]" : "") + "}";
        }
        meshes += (mi ? "," : "") + std::string("{\"name\":\"") + m.name + "\",\"primitives\":[" + prims + "]}";
    }
    for (size_t i = 0; i < s.materials.size(); ++i) {
        const Float4 c = s.materials[i].baseColorFactor;
        char fc[160]; std::snprintf(fc, sizeof fc, "[%g,%g,%g,%g]", c.x, c.y, c.z, c.w);
        materials += (i ? "," : "") + std::string("{\"name\":\"") + s.materials[i].name +
                     "\",\"pbrMetallicRoughness\":{\"baseColorFactor\":" + fc + ",\"roughnessFactor\":0.7,\"metallicFactor\":0}}";
    }
    auto matrix = [](const Float4x4& m) {
        std::string out = "[";
        for (int k = 0; k < 16; ++k) { char v[32]; std::snprintf(v, sizeof v, "%s%.9g", k ? "," : "", m.m[k]); out += v; }
        return out + "]";
    };
    // Scene nodes. The root carries the authoring conversion; skinned meshes are
    // not on it (they hang from their own skin nodes, listed next).
    for (size_t ni = 0; ni < N; ++ni) {
        const Node& n = s.nodes[ni];
        std::string kids;
        auto kid = [&](size_t k) { kids += (kids.empty() ? "" : ",") + std::to_string(k); };
        for (size_t c = 0; c < N; ++c) if (s.nodes[c].parent == (int32_t)ni) kid(c);
        if (ni == 0) {
            for (size_t k = 0; k < skinnedMeshes.size(); ++k) kid(N + k);
            for (size_t bi = 0; bi < boneCount; ++bi) if (s.skeleton->bones[bi].parent < 0) kid(firstBone + bi);
            if (x.camera) kid(firstBone + boneCount);
        }
        int mesh = -1;
        for (uint32_t mi : n.meshes) if (!s.meshes[mi].skinned()) mesh = (int)mi;
        nodes += (ni ? "," : "") + std::string("{\"name\":\"") + n.name + "\",\"matrix\":" +
                 matrix(ni == 0 ? mul(f.c, n.local) : f.transform(n.local)) +
                 (mesh >= 0 ? ",\"mesh\":" + std::to_string(mesh) : "") + (kids.empty() ? "" : ",\"children\":[" + kids + "]") + "}";
    }
    for (size_t k = 0; k < skinnedMeshes.size(); ++k)
        nodes += ",{\"name\":\"" + s.meshes[skinnedMeshes[k]].name + " skin\",\"mesh\":" + std::to_string(skinnedMeshes[k]) + ",\"skin\":0}";
    if (s.skeleton) {
        std::vector<float> ibm;
        std::string jointList;
        for (size_t bi = 0; bi < boneCount; ++bi) {
            const Bone& bone = s.skeleton->bones[bi];
            const Float4x4 L = f.transform(bone.bindLocal);
            const Float4x4 I = f.transform(bone.inverseBind);
            ibm.insert(ibm.end(), I.m, I.m + 16);
            jointList += (bi ? "," : "") + std::to_string(firstBone + bi);
            std::string kids;
            for (size_t c = 0; c < boneCount; ++c)
                if (s.skeleton->bones[c].parent == (int32_t)bi) kids += (kids.empty() ? "" : ",") + std::to_string(firstBone + c);
            char t[128]; std::snprintf(t, sizeof t, "[%.9g,%.9g,%.9g]", L.m[12], L.m[13], L.m[14]);
            nodes += ",{\"name\":\"" + bone.name + "\",\"translation\":" + t + (kids.empty() ? "" : ",\"children\":[" + kids + "]") + "}";
        }
        const int ibmAcc = b.floats(ibm.data(), boneCount, 16, "MAT4");
        skins = "[{\"joints\":[" + jointList + "],\"inverseBindMatrices\":" + std::to_string(ibmAcc) + "}]";
        for (size_t ci = 0; ci < s.clips.size(); ++ci) {
            const Clip& c = s.clips[ci];
            std::string samplers, channels;
            int n = 0;
            for (const Track& tr : c.tracks) {
                size_t bi = 0; while (s.skeleton->bones[bi].name != tr.bone) ++bi;
                std::vector<float> times, values;
                for (const auto& k : tr.rotation) {
                    const Quat q = f.rotation(k.value);
                    times.push_back(k.time);
                    // CUBICSPLINE stores (in-tangent, value, out-tangent) per key; the
                    // tangents are written as garbage so reading the wrong one shows.
                    if (x.spline) values.insert(values.end(), {9, 9, 9, 9});
                    values.insert(values.end(), {q.x, q.y, q.z, q.w});
                    if (x.spline) values.insert(values.end(), {-9, -9, -9, -9});
                }
                const int in = b.floats(times.data(), times.size(), 1, "SCALAR", true);
                const int out = b.floats(values.data(), values.size() / 4, 4, "VEC4");
                samplers += (n ? "," : "") + std::string("{\"input\":") + std::to_string(in) + ",\"output\":" + std::to_string(out) +
                            (x.spline ? ",\"interpolation\":\"CUBICSPLINE\"" : "") + "}";
                channels += (n ? "," : "") + std::string("{\"sampler\":") + std::to_string(n) + ",\"target\":{\"node\":" +
                            std::to_string(firstBone + bi) + ",\"path\":\"rotation\"}}";
                ++n;
            }
            animations += (ci ? "," : "") + std::string("{\"name\":\"") + c.name + "\",\"samplers\":[" + samplers + "],\"channels\":[" + channels + "]}";
        }
    }
    if (x.camera) nodes += ",{\"name\":\"Cam\",\"camera\":0}";
    while (b.data.size() % 4) b.data.push_back(0);
    std::string j = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[" + nodes + "]";
    if (!meshes.empty()) j += ",\"meshes\":[" + meshes + "]";
    if (!materials.empty()) j += ",\"materials\":[" + materials + "]";
    if (!skins.empty()) j += ",\"skins\":" + skins;
    if (!animations.empty()) j += ",\"animations\":[" + animations + "]";
    if (x.camera) j += ",\"cameras\":[{\"type\":\"perspective\",\"perspective\":{\"yfov\":1.0,\"znear\":0.1}}]";
    if (!b.data.empty())
        j += ",\"accessors\":[" + b.accessors + "],\"bufferViews\":[" + b.views + "],\"buffers\":[{\"byteLength\":" +
             std::to_string(b.data.size()) + ",\"uri\":\"data:application/octet-stream;base64," + base64(b.data) + "\"}]";
    return j + "}";
}

}  // namespace gltfw
