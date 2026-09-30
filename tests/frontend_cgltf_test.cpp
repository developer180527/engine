// ── frontend_cgltf_test — the cgltf front end against the contract suite (WO-012)
//
// Each reference case of tests/import_contract.h is WRITTEN as a real .gltf
// file (JSON with an embedded base64 buffer) from the case's own expected
// scene, then imported by CgltfFrontend and compared by meaning. So the file
// the front end reads says exactly what the suite expects back, in glTF.
//
// Nothing is skipped: glTF expresses every case, skins and clips included
// (WO-014). The centimetre/Z-up case puts its conversion in a root node, the
// way an exporter does, since glTF declares neither units nor an up axis.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "assets/import/frontend_cgltf.h"
#include "import_contract.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL  " __VA_ARGS__); std::printf("\n"); ++g_failures; } \
                           else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } } while (0)

namespace fs = std::filesystem;
using namespace imp;
using impcontract::Case;

// ── A minimal glTF writer, for tests ────────────────────────────────────────
namespace gltfw {

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

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("frontend_cgltf_test\n");
    const fs::path dir = fs::temp_directory_path() / "wo012_frontend_cgltf";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto put = [&](const std::string& name, const std::string& json) {
        std::ofstream(dir / name) << json;
        return dir / name;
    };

    // ── 1. The contract suite ────────────────────────────────────────────────
    std::printf("1. the import-frontend contract suite\n");
    CgltfFrontend fe;
    impcontract::Subject subj{"cgltf", &fe, [&](Case c) -> std::optional<fs::path> {
        switch (c) {
            case Case::AuthoredCentimetreZUp:                              // conversion in the root node
                return put("AuthoredCentimetreZUp.gltf", gltfw::write(impcontract::expected(c), {}, {.unit = 0.01f, .zUp = true}));
            case Case::Unrepresentable:
                return put("Unrepresentable.gltf",
                           gltfw::write(impcontract::expected(c), {.morph = true, .colours = true, .camera = true}));
            case Case::EmptyFile:
                return put("EmptyFile.gltf", "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
                                             "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"name\":\"empty\"}]}");
            default:
                return put(std::string(impcontract::name(c)) + ".gltf", gltfw::write(impcontract::expected(c)));
        }
    }};
    const impcontract::Report r = impcontract::run(subj);
    for (const auto& f : r.failures) std::printf("        %s\n", f.c_str());
    CHECK(r.failures.empty(), "no case fails (%d passed)", r.passed);
    CHECK(r.skipped.empty(), "none skipped: glTF skins and clips are read (WO-014) (%zu skipped)", r.skipped.size());

    // ── 2. What the front end drops, and with what effect ───────────────────
    std::printf("2. the dropped list\n");
    {
        // WO-002 refused a skinned glTF; since WO-014 it is read. The skinned
        // reference scene, written as glTF, imports with its skeleton, weights
        // and clip, and no skin loss.
        const ImportResult s = fe.importScene(put("skinned.gltf", gltfw::write(impcontract::expected(Case::SkinnedColumn))), {});
        bool skinLoss = false, hips = false, spine = false;
        if (s) for (const auto& d : s.scene().dropped) skinLoss |= d.kind == Dropped::Kind::Skin;
        if (s && s.scene().skeleton)                            // plus the file's root, kept as an ancestor
            for (const Bone& bone : s.scene().skeleton->bones) { hips |= bone.name == "Hips"; spine |= bone.name == "Spine"; }
        CHECK(s && hips && spine && s.scene().meshes[0].skinned() &&
              s.scene().clips.size() == 1 && !skinLoss,
              "a skinned glTF is read, skeleton, weights and clip included (WO-002's refusal is gone)");

        // A cubic-spline clip: its keyed VALUES are played (the middle of each
        // in-tangent/value/out-tangent triple), and the lost tangents are reported.
        const ImportedScene want = impcontract::expected(Case::SkinnedColumn);
        const ImportResult sp = fe.importScene(put("spline.gltf", gltfw::write(want, {.spline = true})), {});
        std::vector<std::string> why;
        bool splineNoted = false;
        if (sp) {
            impcontract::detail::compareClips(sp.scene(), want, why);
            for (const auto& d : sp.scene().dropped)
                splineNoted |= d.kind == Dropped::Kind::Animation && d.what.find("cubic-spline") != std::string::npos;
        }
        CHECK(sp && why.empty() && splineNoted, "a cubic-spline clip plays its keyed values, and its dropped tangents are reported%s",
              why.empty() ? "" : (": " + why[0]).c_str());

        // A texture that does not resolve is dropped with its path, never guessed at.
        ImportedScene tri = impcontract::expected(Case::UnitTriangle);
        std::string j = gltfw::write(tri);
        j.insert(j.rfind('}'), ",\"images\":[{\"uri\":\"nowhere.png\"}],\"textures\":[{\"source\":0}]");
        j.replace(j.find("\"pbrMetallicRoughness\":{"), std::strlen("\"pbrMetallicRoughness\":{"),
                  "\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":0},");
        const ImportResult t = fe.importScene(put("missing_texture.gltf", j), {});
        bool missing = false;
        if (t) for (const auto& d : t.scene().dropped)
            missing |= d.kind == Dropped::Kind::Texture && d.effect == Dropped::Effect::Less && d.what.find("nowhere.png") != std::string::npos;
        CHECK(t && missing && t.scene().materials[0].baseColor.empty(),
              "an unresolvable texture is dropped, naming it, and the slot is left empty");
    }

    // ── 3. Things the old cook path got wrong ───────────────────────────────
    std::printf("3. fixes over the old cook path\n");
    {
        // A primitive with no indices: the old path skipped it without a word.
        std::string j = gltfw::write(impcontract::expected(Case::UnitTriangle));
        const size_t at = j.find(",\"indices\":");
        j.erase(at, j.find(',', at + 1) - at);
        const ImportResult r2 = fe.importScene(put("non_indexed.gltf", j), {});
        CHECK(r2 && r2.scene().meshes.size() == 1 && r2.scene().meshes[0].indices == std::vector<uint32_t>({0, 1, 2}),
              "a non-indexed primitive is imported with sequential indices");

        // A primitive with no material: a real default, not "material 0".
        ImportedScene quad = impcontract::expected(Case::TwoMaterialQuad);
        std::string q = gltfw::write(quad);
        const size_t m1 = q.find(",\"material\":1");
        q.erase(m1, std::strlen(",\"material\":1"));
        const ImportResult r3 = fe.importScene(put("no_material.gltf", q), {});
        CHECK(r3 && r3.scene().materials.size() == 3 && r3.scene().materials[2].name == "default" &&
              r3.scene().meshes[1].submeshes[0].material == 2,
              "a primitive with no material gets an appended default, not the file's first material");
    }

    fs::remove_all(dir);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
