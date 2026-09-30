#pragma once
// ── import_contract — the suite every import front end must pass (WO-010) ─────
//
// docs/contracts/import-frontend.md says what a front end promises; this is how
// that promise is checked, ONCE, for every front end: the fake today, cgltf
// (WO-012) and Assimp (WO-013) next. A front end's test supplies a Subject:
// itself, plus a way to produce a SOURCE FILE that expresses each reference
// case in its own format. The suite imports it and compares the result with
// the case's expected scene, which is written in engine conventions.
//
// ── Compared by MEANING, not by bytes ───────────────────────────────────────
// A real parser may reorder vertices, weld them, split them, or keep or flatten
// node hierarchies. None of that changes what the file means, so none of it may
// fail the suite. What is compared:
//   * world-space triangles: each corner's position, normal, UV, and dominant
//     bone and weight, plus the triangle's material. Triangles are canonicalised
//     by ROTATING their corners, never reordering them, so a flipped winding is
//     still caught;
//   * the skeleton by bone name: parent name and bind world matrix;
//   * clips by name: duration, and each track's first and last key;
//   * the dropped list as a multiset of (kind, effect).
// Structural invariants come from imp::checkScene on every result.
//
// A case a format cannot express (OBJ has no skins) is SKIPPED and reported,
// never counted as passed. The front end's own test decides which skips it
// accepts.
#include "assets/import/fake_frontend.h"
#include "assets/import/import_frontend.h"
#include "assets/import/imported_scene.h"
#include "assets/import/imported_scene_check.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace impcontract {

using namespace imp;

// ── The reference cases ─────────────────────────────────────────────────────
enum class Case {
    UnitTriangle,          // one triangle, CCW, +Z normal: winding, normals, UVs
    NodeTransforms,        // one mesh on two nodes, a parent chain with rotation: hierarchy, instancing
    TwoMaterialQuad,       // one mesh, two submeshes, two materials: submesh ranges
    SkinnedColumn,         // two bones, blended weights, one clip: skeleton, weights, clips
    AuthoredCentimetreZUp, // SkinnedColumn authored 100 cm tall, Z up: units and axes (plan §4, Q1)
    Unrepresentable,       // UnitTriangle plus morph targets, vertex colours, a camera: the dropped list
    EmptyFile,             // parses, holds nothing: ImportError::Empty
};
inline constexpr Case kAllCases[] = {Case::UnitTriangle, Case::NodeTransforms, Case::TwoMaterialQuad,
                                     Case::SkinnedColumn, Case::AuthoredCentimetreZUp,
                                     Case::Unrepresentable, Case::EmptyFile};

inline const char* name(Case c) {
    switch (c) {
        case Case::UnitTriangle:          return "UnitTriangle";
        case Case::NodeTransforms:        return "NodeTransforms";
        case Case::TwoMaterialQuad:       return "TwoMaterialQuad";
        case Case::SkinnedColumn:         return "SkinnedColumn";
        case Case::AuthoredCentimetreZUp: return "AuthoredCentimetreZUp";
        case Case::Unrepresentable:       return "Unrepresentable";
        case Case::EmptyFile:             return "EmptyFile";
    }
    return "?";
}

namespace build {
inline Float4x4 translation(float x, float y, float z) { Float4x4 t; t.m[12] = x; t.m[13] = y; t.m[14] = z; return t; }
inline Float4x4 rotationY90() {          // +X -> -Z, +Z -> +X
    Float4x4 r; r.m[0] = 0; r.m[2] = -1; r.m[8] = 1; r.m[10] = 0; return r;
}
inline Material material(const char* n, Float4 c) { Material m; m.name = n; m.baseColorFactor = c; return m; }
inline Node node(const char* n, int32_t parent, Float4x4 local, std::vector<uint32_t> meshes) {
    Node nd; nd.name = n; nd.parent = parent; nd.local = local; nd.meshes = std::move(meshes); return nd;
}
inline Mesh triangle() {
    Mesh m; m.name = "Triangle";
    m.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    m.normals   = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
    m.uv0       = {{0, 1}, {1, 1}, {0, 0}};          // top-left origin: v grows downward
    m.indices   = {0, 1, 2};                        // CCW seen from +Z, the normal's side
    m.submeshes = {{0, 3, 0}};
    return m;
}
}  // namespace build

// What importing each case must produce, in engine conventions.
inline ImportedScene expected(Case c) {
    using namespace build;
    ImportedScene s;
    switch (c) {
        case Case::UnitTriangle:
        case Case::Unrepresentable:
            s.meshes    = {triangle()};
            s.materials = {material("White", {1, 1, 1, 1})};
            s.nodes     = {node("root", -1, {}, {0})};
            if (c == Case::Unrepresentable)
                s.dropped = {{Dropped::Kind::MorphTargets,  Dropped::Effect::Less, 1, "2 morph targets on 'Triangle'"},
                             {Dropped::Kind::VertexColours, Dropped::Effect::Less, 1, "COLOR_0 on 'Triangle'"},
                             {Dropped::Kind::Camera,        Dropped::Effect::Less, 1, "camera 'Cam'"}};
            break;

        case Case::NodeTransforms:              // the same mesh twice: at (0,2,0), and turned 90° about Y under it
            s.meshes    = {triangle()};
            s.materials = {material("White", {1, 1, 1, 1})};
            s.nodes     = {node("root", -1, {}, {}),
                           node("Raised", 0, translation(0, 2, 0), {0}),
                           node("Turned", 1, rotationY90(), {0})};
            break;

        case Case::TwoMaterialQuad: {
            Mesh m; m.name = "Quad";
            m.positions = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
            m.normals   = {{0, 0, 1}, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}};
            m.uv0       = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
            m.indices   = {0, 1, 2,  0, 2, 3};
            m.submeshes = {{0, 3, 0}, {3, 3, 1}};
            s.meshes    = {m};
            s.materials = {material("Red", {1, 0, 0, 1}), material("Blue", {0, 0, 1, 1})};
            s.nodes     = {node("root", -1, {}, {0})};
            break;
        }

        case Case::SkinnedColumn:
        case Case::AuthoredCentimetreZUp: {       // same meaning: 1 m tall, +Y up
            Mesh m; m.name = "Column";
            m.positions = {{-0.1f, 0, 0}, {0.1f, 0, 0}, {-0.1f, 0.5f, 0}, {0.1f, 0.5f, 0},
                           {-0.1f, 1, 0}, {0.1f, 1, 0}};
            m.normals.assign(6, {0, 0, 1});
            m.uv0       = {{0, 1}, {1, 1}, {0, 0.5f}, {1, 0.5f}, {0, 0}, {1, 0}};
            m.joints.assign(6, {0, 1, 0, 0});
            m.weights   = {{1, 0, 0, 0}, {1, 0, 0, 0}, {0.5f, 0.5f, 0, 0}, {0.5f, 0.5f, 0, 0},
                           {0, 1, 0, 0}, {0, 1, 0, 0}};
            m.indices   = {0, 1, 3,  0, 3, 2,  2, 3, 5,  2, 5, 4};
            m.submeshes = {{0, 12, 0}};
            s.meshes    = {m};
            s.materials = {material("Skin", {1, 1, 1, 1})};
            s.nodes     = {node("root", -1, {}, {0})};
            Skeleton sk;
            sk.bones = {{"Hips",  -1, {},                          {}},
                        {"Spine",  0, translation(0, 0.5f, 0),      translation(0, -0.5f, 0)}};
            s.skeleton = sk;
            Clip bend; bend.name = "Bend"; bend.duration = 1.0f;
            Track t; t.bone = "Spine";
            t.rotation = {{0.0f, {0, 0, 0, 1}}, {1.0f, {0, 0, 0.38268343f, 0.92387953f}}};   // 0 -> 45° about Z
            bend.tracks = {t};
            s.clips = {bend};
            break;
        }

        case Case::EmptyFile: break;
    }
    return s;
}

// ── A front end under test ──────────────────────────────────────────────────
struct Subject {
    std::string name;
    const IImportFrontend* frontend = nullptr;
    // A source file in this front end's format that expresses the case, or
    // nullopt if the format cannot express it (reported as SKIPPED).
    std::function<std::optional<std::filesystem::path>(Case)> source;
};

struct Report {
    int passed = 0;
    std::vector<std::string> failures;   // "Case: what"
    std::vector<std::string> skipped;    // case names

    bool failedCase(Case c) const {
        const std::string p = std::string(name(c)) + ":";
        for (const auto& f : failures) if (f.rfind(p, 0) == 0) return true;
        return false;
    }
};

// ── Comparison by meaning ───────────────────────────────────────────────────
namespace detail {

struct Corner { Float3 p, n; Float2 uv; std::string bone; float weight = 0; };
struct Tri    { Corner c[3]; std::string material; Float4 colour; };

inline float q(float v) { return std::round(v * 1000.0f) / 1000.0f; }  // compare at 1 mm / 1e-3
inline bool cornerLess(const Corner& a, const Corner& b) {
    if (q(a.p.x) != q(b.p.x)) return q(a.p.x) < q(b.p.x);
    if (q(a.p.y) != q(b.p.y)) return q(a.p.y) < q(b.p.y);
    return q(a.p.z) < q(b.p.z);
}
inline Float3 dir(const Float4x4& t, Float3 v) {
    Float3 r{t.m[0] * v.x + t.m[4] * v.y + t.m[8] * v.z,
             t.m[1] * v.x + t.m[5] * v.y + t.m[9] * v.z,
             t.m[2] * v.x + t.m[6] * v.y + t.m[10] * v.z};
    const float l = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
    return l > 1e-8f ? Float3{r.x / l, r.y / l, r.z / l} : r;
}

// Every triangle of every mesh on every node, in world space, winding kept.
inline std::vector<Tri> soup(const ImportedScene& s) {
    std::vector<Tri> out;
    const auto* bones = s.skeleton ? &s.skeleton->bones : nullptr;
    for (size_t ni = 0; ni < s.nodes.size(); ++ni) {
        const Float4x4 w = worldOf(s.nodes, ni, &Node::local);
        for (uint32_t mi : s.nodes[ni].meshes) {
            const Mesh& m = s.meshes[mi];
            for (const Submesh& sm : m.submeshes)
                for (uint32_t i = sm.firstIndex; i + 3 <= sm.firstIndex + sm.indexCount; i += 3) {
                    Tri t;
                    const Material& mat = s.materials[sm.material];
                    t.material = mat.name; t.colour = mat.baseColorFactor;
                    for (int k = 0; k < 3; ++k) {
                        const uint32_t v = m.indices[i + k];
                        Corner& c = t.c[k];
                        c.p  = transformPoint(w, m.positions[v]);
                        c.n  = dir(w, m.normals[v]);
                        c.uv = m.uv0.empty() ? Float2{} : m.uv0[v];
                        if (!m.weights.empty() && bones) {
                            const float ws[4] = {m.weights[v].x, m.weights[v].y, m.weights[v].z, m.weights[v].w};
                            int best = 0;
                            for (int j = 1; j < 4; ++j) if (ws[j] > ws[best]) best = j;
                            // A tie (0.5/0.5) is resolved by bone NAME so two importers agree.
                            for (int j = 0; j < 4; ++j)
                                if (j != best && std::fabs(ws[j] - ws[best]) < 1e-4f &&
                                    (*bones)[m.joints[v][j]].name < (*bones)[m.joints[v][best]].name) best = j;
                            c.bone = (*bones)[m.joints[v][best]].name;
                            c.weight = ws[best];
                        }
                    }
                    // Rotate (never reorder) so the smallest corner leads: winding survives.
                    int lead = 0;
                    for (int k = 1; k < 3; ++k) if (cornerLess(t.c[k], t.c[lead])) lead = k;
                    Tri r = t;
                    for (int k = 0; k < 3; ++k) r.c[k] = t.c[(lead + k) % 3];
                    out.push_back(r);
                }
        }
    }
    std::sort(out.begin(), out.end(), [](const Tri& a, const Tri& b) {
        for (int k = 0; k < 3; ++k) {
            if (cornerLess(a.c[k], b.c[k])) return true;
            if (cornerLess(b.c[k], a.c[k])) return false;
        }
        return a.material < b.material;
    });
    return out;
}

inline bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
inline bool near3(Float3 a, Float3 b, float eps = 1e-3f) { return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps); }
inline std::string str(Float3 v) { char b[96]; std::snprintf(b, sizeof b, "(%.4g, %.4g, %.4g)", v.x, v.y, v.z); return b; }

inline void compareGeometry(const ImportedScene& got, const ImportedScene& want, std::vector<std::string>& why) {
    const auto g = soup(got), w = soup(want);
    if (g.size() != w.size()) { why.push_back("triangles: got " + std::to_string(g.size()) + ", want " + std::to_string(w.size())); return; }
    for (size_t i = 0; i < w.size(); ++i) {
        for (int k = 0; k < 3; ++k) {
            const Corner &a = g[i].c[k], &b = w[i].c[k];
            if (!near3(a.p, b.p)) { why.push_back("triangle " + std::to_string(i) + " corner " + std::to_string(k) + ": position " + str(a.p) + ", want " + str(b.p) + " (winding, units, axes or node transforms)"); return; }
            if (!near3(a.n, b.n)) { why.push_back("triangle " + std::to_string(i) + ": normal " + str(a.n) + ", want " + str(b.n)); return; }
            if (!near(a.uv.x, b.uv.x) || !near(a.uv.y, b.uv.y)) { why.push_back("triangle " + std::to_string(i) + ": UV differs (origin must be top-left)"); return; }
            if (a.bone != b.bone || !near(a.weight, b.weight)) { why.push_back("triangle " + std::to_string(i) + ": dominant bone '" + a.bone + "' " + std::to_string(a.weight) + ", want '" + b.bone + "' " + std::to_string(b.weight)); return; }
        }
        if (g[i].material != w[i].material || !near(g[i].colour.x, w[i].colour.x) ||
            !near(g[i].colour.y, w[i].colour.y) || !near(g[i].colour.z, w[i].colour.z) || !near(g[i].colour.w, w[i].colour.w)) {
            why.push_back("triangle " + std::to_string(i) + ": material '" + g[i].material + "', want '" + w[i].material + "'"); return;
        }
    }
}

inline void compareSkeleton(const ImportedScene& got, const ImportedScene& want, std::vector<std::string>& why) {
    if (!want.skeleton) { if (got.skeleton) why.push_back("skeleton: none expected"); return; }
    if (!got.skeleton)  { why.push_back("skeleton: missing"); return; }
    const auto &g = got.skeleton->bones, &w = want.skeleton->bones;
    if (g.size() != w.size()) { why.push_back("skeleton: " + std::to_string(g.size()) + " bones, want " + std::to_string(w.size())); return; }
    auto parentName = [](const std::vector<Bone>& bs, const Bone& b) { return b.parent < 0 ? std::string() : bs[(size_t)b.parent].name; };
    for (size_t wi = 0; wi < w.size(); ++wi) {
        auto it = std::find_if(g.begin(), g.end(), [&](const Bone& b) { return b.name == w[wi].name; });
        if (it == g.end()) { why.push_back("skeleton: no bone '" + w[wi].name + "'"); return; }
        const size_t gi = (size_t)(it - g.begin());
        if (parentName(g, *it) != parentName(w, w[wi])) { why.push_back("skeleton: '" + w[wi].name + "' parent '" + parentName(g, *it) + "', want '" + parentName(w, w[wi]) + "'"); return; }
        const Float3 o{0, 0, 0};
        const Float3 gp = transformPoint(worldOf(g, gi, &Bone::bindLocal), o), wp = transformPoint(worldOf(w, wi, &Bone::bindLocal), o);
        if (!near3(gp, wp)) { why.push_back("skeleton: '" + w[wi].name + "' bind position " + str(gp) + ", want " + str(wp)); return; }
    }
}

inline void compareClips(const ImportedScene& got, const ImportedScene& want, std::vector<std::string>& why) {
    if (got.clips.size() != want.clips.size()) { why.push_back("clips: " + std::to_string(got.clips.size()) + ", want " + std::to_string(want.clips.size())); return; }
    for (const Clip& wc : want.clips) {
        auto gc = std::find_if(got.clips.begin(), got.clips.end(), [&](const Clip& c) { return c.name == wc.name; });
        if (gc == got.clips.end()) { why.push_back("clips: no clip '" + wc.name + "'"); return; }
        if (!near(gc->duration, wc.duration)) { why.push_back("clips: '" + wc.name + "' lasts " + std::to_string(gc->duration) + " s, want " + std::to_string(wc.duration)); return; }
        for (const Track& wt : wc.tracks) {
            auto gt = std::find_if(gc->tracks.begin(), gc->tracks.end(), [&](const Track& t) { return t.bone == wt.bone; });
            if (gt == gc->tracks.end()) { why.push_back("clips: '" + wc.name + "' does not animate '" + wt.bone + "'"); return; }
            auto sameRot = [](const Quat& a, const Quat& b) { return std::fabs(std::fabs(a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w) - 1.0f) < 1e-3f; };
            if (!wt.rotation.empty()) {
                if (gt->rotation.empty() || !sameRot(gt->rotation.front().value, wt.rotation.front().value) ||
                    !sameRot(gt->rotation.back().value, wt.rotation.back().value)) {
                    why.push_back("clips: '" + wc.name + "' bone '" + wt.bone + "' rotation differs at the first or last key"); return;
                }
            }
        }
    }
}

inline void compareDropped(const ImportedScene& got, const ImportedScene& want, std::vector<std::string>& why) {
    auto key = [](const Dropped& d) { return std::string(toString(d.kind)) + "/" + toString(d.effect); };
    std::multiset<std::string> g, w;
    for (const auto& d : got.dropped)  g.insert(key(d));
    for (const auto& d : want.dropped) w.insert(key(d));
    if (g == w) return;
    std::string gs, ws;
    for (const auto& k : g) gs += k + " ";
    for (const auto& k : w) ws += k + " ";
    why.push_back("dropped: [" + gs + "], want [" + ws + "] (every loss is reported, with the right effect)");
}

}  // namespace detail

// ── The suite ───────────────────────────────────────────────────────────────
inline Report run(const Subject& subj) {
    Report r;
    auto fail = [&](Case c, const std::string& what) { r.failures.push_back(std::string(name(c)) + ": " + what); };

    for (Case c : kAllCases) {
        const std::optional<std::filesystem::path> src = subj.source(c);
        if (!src) { r.skipped.push_back(name(c)); continue; }
        const ImportResult res = subj.frontend->importScene(*src, {});

        if (c == Case::EmptyFile) {
            if (res) fail(c, "returned a scene; an empty file must be ImportError::Empty");
            else if (res.error().kind != ImportError::Kind::Empty)
                fail(c, std::string("error kind ") + toString(res.error().kind) + ", want empty");
            else ++r.passed;
            continue;
        }
        if (!res) { fail(c, std::string("import failed (") + toString(res.error().kind) + "): " + res.error().message); continue; }

        const ImportedScene& got = res.scene();
        const ImportedScene  want = expected(c);
        std::vector<std::string> why;
        for (const Violation& v : checkScene(got)) why.push_back(std::string("structure/") + v.check + ": " + v.detail);
        if (why.empty()) {                     // meaning only makes sense on a well-formed scene
            detail::compareGeometry(got, want, why);
            detail::compareSkeleton(got, want, why);
            detail::compareClips(got, want, why);
            detail::compareDropped(got, want, why);
        }
        if (why.empty()) ++r.passed;
        for (const auto& w : why) fail(c, w);
    }

    // A file that does not exist is Unreadable, for every front end.
    const auto exts = subj.frontend->extensions();
    const std::filesystem::path missing =
        std::filesystem::path("/nonexistent/wo010-contract/missing").replace_extension(exts.empty() ? "" : "." + exts[0]);
    const ImportResult m = subj.frontend->importScene(missing, {});
    if (m) r.failures.push_back("MissingFile: returned a scene for a file that does not exist");
    else if (m.error().kind != ImportError::Kind::Unreadable)
        r.failures.push_back(std::string("MissingFile: error kind ") + toString(m.error().kind) + ", want unreadable");
    else ++r.passed;
    return r;
}

// The fake's Subject: each case's source is a path the fake answers with the
// expected scene. It passes by construction, which proves the SUITE runs, not
// that importing works. Real front ends answer with real files.
inline Subject fakeSubject(FakeFrontend& fake) {
    for (Case c : kAllCases) {
        const std::string p = std::string(name(c)) + ".fake";
        if (c == Case::EmptyFile) fake.set(p, ImportError{ImportError::Kind::Empty, "nothing to import: " + p});
        else                      fake.set(p, expected(c));
    }
    return {"fake", &fake, [](Case c) { return std::optional<std::filesystem::path>(std::string(name(c)) + ".fake"); }};
}

}  // namespace impcontract
