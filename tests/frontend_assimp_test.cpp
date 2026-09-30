// ── frontend_assimp_test — the Assimp front end against the contract suite ────
//
// WO-013. Each reference case of tests/import_contract.h is WRITTEN as a real
// COLLADA (.dae) file from its expected scene, then imported by AssimpFrontend
// and compared by meaning. COLLADA, because it is text Assimp reads and it can
// say everything the suite asks: node transforms, materials, skins, animation
// clips, morph targets, vertex colours, cameras, and its own units and axes.
//
// AuthoredCentimetreZUp is written in centimetres, Z up, and DECLARED so in
// <asset>; Assimp's COLLADA reader converts the root, and the front end must
// carry that conversion through skeleton and skin. (FBX units are WO-035.)
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "assets/import/frontend_assimp.h"
#include "import_contract.h"
#include "core/thread_stack.h"   // engine::threads::runWithStack

static int g_failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL  " __VA_ARGS__); std::printf("\n"); ++g_failures; } \
                           else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } } while (0)

namespace fs = std::filesystem;
using namespace imp;
using impcontract::Case;

// ── A minimal COLLADA writer, for tests ─────────────────────────────────────
namespace dae {

// A file authored in other units/axes: everything it states is C^-1 of what the
// engine should see, where C is the conversion Assimp applies at the root
// (unit scale, then Z-up -> Y-up: (x, y, z) -> (x, z, -y)).
struct Authoring {
    float unit = 1.0f;       // metres per file unit
    bool  zUp  = false;
};

Float4x4 conversion(const Authoring& a) {          // C, column-major
    Float4x4 c;
    if (a.zUp) { c.m[5] = 0; c.m[6] = -1; c.m[9] = 1; c.m[10] = 0; }   // columns: X->X, Y->-Z, Z->Y
    for (int i : {0, 1, 2, 4, 5, 6, 8, 9, 10}) c.m[i] *= a.unit;
    return c;
}
// Written in the file: C^-1 * M * C for a transform, C^-1 * p for a point.
struct Frame {
    Float4x4 c, ci;
    explicit Frame(const Authoring& a) : c(conversion(a)), ci(inverse(conversion(a))) {}
    Float4x4 transform(const Float4x4& m) const { return mul(ci, mul(m, c)); }
    Float3 point(Float3 p) const { return transformPoint(ci, p); }
    Float3 dir(Float3 v) const {
        Float3 r = sub0(transformPoint(ci, v)); const float l = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z);
        return {r.x / l, r.y / l, r.z / l};
    }
    Float3 sub0(Float3 p) const { const Float3 o = transformPoint(ci, {0, 0, 0}); return {p.x - o.x, p.y - o.y, p.z - o.z}; }
};

std::string rowMajor(const Float4x4& m) {
    std::ostringstream o; o.precision(9);
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) o << m.m[c * 4 + r] << ' ';
    return o.str();
}
std::string floats(const std::vector<float>& v) {
    std::ostringstream o; o.precision(9);
    for (float f : v) o << f << ' ';
    return o.str();
}
std::string source(const std::string& id, const std::vector<float>& v, int stride, const char* params) {
    std::string p;
    for (const char* c = params; *c; ++c) p += std::string("<param name=\"") + *c + "\" type=\"float\"/>";
    return "<source id=\"" + id + "\"><float_array id=\"" + id + "-a\" count=\"" + std::to_string(v.size()) + "\">" +
           floats(v) + "</float_array><technique_common><accessor source=\"#" + id + "-a\" count=\"" +
           std::to_string(v.size() / stride) + "\" stride=\"" + std::to_string(stride) + "\">" + p + "</accessor></technique_common></source>";
}

struct Extras { bool morph = false, colours = false, camera = false; };

std::string write(const ImportedScene& s, Authoring au = {}, Extras x = {}) {
    const Frame f(au);
    std::string effects, materials, geometries, controllers, animations, nodes;

    for (const Material& m : s.materials) {
        char col[128]; std::snprintf(col, sizeof col, "%g %g %g %g", m.baseColorFactor.x, m.baseColorFactor.y, m.baseColorFactor.z, m.baseColorFactor.w);
        effects   += "<effect id=\"" + m.name + "-fx\"><profile_COMMON><technique sid=\"common\"><lambert><diffuse><color>" +
                     col + "</color></diffuse></lambert></technique></profile_COMMON></effect>";
        materials += "<material id=\"" + m.name + "\" name=\"" + m.name + "\"><instance_effect url=\"#" + m.name + "-fx\"/></material>";
    }
    auto bindMaterials = [&](const Mesh& m) {
        std::string b = "<bind_material><technique_common>";
        for (const Submesh& sm : m.submeshes)
            b += "<instance_material symbol=\"" + s.materials[sm.material].name + "\" target=\"#" + s.materials[sm.material].name + "\"/>";
        return b + "</technique_common></bind_material>";
    };

    for (size_t mi = 0; mi < s.meshes.size(); ++mi) {
        const Mesh& m = s.meshes[mi];
        const std::string g = m.name + "-mesh";
        std::vector<float> pos, nrm, uv, col;
        for (size_t v = 0; v < m.positions.size(); ++v) {
            const Float3 p = f.point(m.positions[v]), n = f.dir(m.normals[v]);
            pos.insert(pos.end(), {p.x, p.y, p.z}); nrm.insert(nrm.end(), {n.x, n.y, n.z});
            if (!m.uv0.empty()) uv.insert(uv.end(), {m.uv0[v].x, 1.0f - m.uv0[v].y});   // COLLADA's V grows upward
            col.insert(col.end(), {1, 1, 1, 1});
        }
        // Every reference inside a geometry is built from ITS id, so a morph
        // target (a second geometry) never points into the base one.
        auto triangles = [&](const std::string& id) {
            std::string tris;
            for (const Submesh& sm : m.submeshes) {
                std::string p;
                for (uint32_t i = sm.firstIndex; i < sm.firstIndex + sm.indexCount; ++i) p += std::to_string(m.indices[i]) + ' ';
                tris += "<triangles material=\"" + s.materials[sm.material].name + "\" count=\"" + std::to_string(sm.indexCount / 3) +
                        "\"><input semantic=\"VERTEX\" source=\"#" + id + "-v\" offset=\"0\"/><input semantic=\"NORMAL\" source=\"#" + id +
                        "-n\" offset=\"0\"/>" + (uv.empty() ? "" : "<input semantic=\"TEXCOORD\" source=\"#" + id + "-uv\" offset=\"0\" set=\"0\"/>") +
                        (x.colours ? "<input semantic=\"COLOR\" source=\"#" + id + "-c\" offset=\"0\"/>" : "") + "<p>" + p + "</p></triangles>";
            }
            return tris;
        };
        auto geometry = [&](const std::string& id) {
            return "<geometry id=\"" + id + "\" name=\"" + m.name + "\"><mesh>" + source(id + "-p", pos, 3, "XYZ") + source(id + "-n", nrm, 3, "XYZ") +
                   (uv.empty() ? "" : source(id + "-uv", uv, 2, "ST")) + (x.colours ? source(id + "-c", col, 4, "RGBA") : "") +
                   "<vertices id=\"" + id + "-v\"><input semantic=\"POSITION\" source=\"#" + id + "-p\"/></vertices>" +
                   triangles(id) + "</mesh></geometry>";
        };
        geometries += geometry(g);
        if (x.morph) {                                       // one morph target, identical to the base
            geometries += geometry(g + "-target");
            controllers += "<controller id=\"" + g + "-morph\"><morph source=\"#" + g + "\" method=\"NORMALIZED\">"
                           "<source id=\"" + g + "-mt\"><IDREF_array id=\"" + g + "-mt-a\" count=\"1\">" + g + "-target</IDREF_array>"
                           "<technique_common><accessor source=\"#" + g + "-mt-a\" count=\"1\" stride=\"1\"><param name=\"IDREF\" type=\"IDREF\"/></accessor></technique_common></source>"
                           + source(g + "-mw", {0.0f}, 1, "W") +
                           "<targets><input semantic=\"MORPH_TARGET\" source=\"#" + g + "-mt\"/><input semantic=\"MORPH_WEIGHT\" source=\"#" + g + "-mw\"/></targets></morph></controller>";
        }
        if (m.skinned()) {                                   // skin controller over the geometry
            const auto& bones = s.skeleton->bones;
            std::string names, ibm, vcount, v; std::vector<float> weights;
            for (size_t b = 0; b < bones.size(); ++b) { names += bones[b].name + ' '; ibm += rowMajor(f.transform(bones[b].inverseBind)); }
            for (size_t vi = 0; vi < m.positions.size(); ++vi) {
                const float w[4] = {m.weights[vi].x, m.weights[vi].y, m.weights[vi].z, m.weights[vi].w};
                int n = 0;
                for (int k = 0; k < 4; ++k) if (w[k] > 0) { v += std::to_string(m.joints[vi][k]) + ' ' + std::to_string(weights.size()) + ' '; weights.push_back(w[k]); ++n; }
                vcount += std::to_string(n) + ' ';
            }
            std::vector<float> ibmf; { std::istringstream is(ibm); float q; while (is >> q) ibmf.push_back(q); }
            controllers += "<controller id=\"" + g + "-skin\"><skin source=\"#" + g + "\"><bind_shape_matrix>1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1</bind_shape_matrix>"
                           "<source id=\"" + g + "-j\"><Name_array id=\"" + g + "-j-a\" count=\"" + std::to_string(bones.size()) + "\">" + names + "</Name_array>"
                           "<technique_common><accessor source=\"#" + g + "-j-a\" count=\"" + std::to_string(bones.size()) + "\" stride=\"1\"><param name=\"JOINT\" type=\"name\"/></accessor></technique_common></source>"
                           "<source id=\"" + g + "-ib\"><float_array id=\"" + g + "-ib-a\" count=\"" + std::to_string(ibmf.size()) + "\">" + ibm + "</float_array>"
                           "<technique_common><accessor source=\"#" + g + "-ib-a\" count=\"" + std::to_string(bones.size()) + "\" stride=\"16\"><param name=\"TRANSFORM\" type=\"float4x4\"/></accessor></technique_common></source>"
                           + source(g + "-w", weights, 1, "W") +
                           "<joints><input semantic=\"JOINT\" source=\"#" + g + "-j\"/><input semantic=\"INV_BIND_MATRIX\" source=\"#" + g + "-ib\"/></joints>"
                           "<vertex_weights count=\"" + std::to_string(m.positions.size()) + "\"><input semantic=\"JOINT\" source=\"#" + g + "-j\" offset=\"0\"/>"
                           "<input semantic=\"WEIGHT\" source=\"#" + g + "-w\" offset=\"1\"/><vcount>" + vcount + "</vcount><v>" + v + "</v></vertex_weights></skin></controller>";
        }
    }

    // Clips: ONE <animation name="…"> per clip, holding every track's sources,
    // sampler and channel (matrix keys, bind translation kept). Assimp names an
    // aiAnimation from the name of the <animation> that holds the channels; it
    // misreads <library_animation_clips> (it takes the LIBRARY's name and wants
    // <instance_animation> directly under it), so clips are not written that way.
    for (const Clip& c : s.clips) {
        std::string body;
        for (const Track& t : c.tracks) {
            const auto& bones = s.skeleton->bones;
            size_t bi = 0; while (bones[bi].name != t.bone) ++bi;
            std::vector<float> times, mats;
            for (const auto& k : t.rotation) {
                const Quat q = k.value;
                Float4x4 r;                               // rotation from the key, translation from the bind pose
                r.m[0] = 1 - 2 * (q.y * q.y + q.z * q.z); r.m[1] = 2 * (q.x * q.y + q.z * q.w);     r.m[2] = 2 * (q.x * q.z - q.y * q.w);
                r.m[4] = 2 * (q.x * q.y - q.z * q.w);     r.m[5] = 1 - 2 * (q.x * q.x + q.z * q.z); r.m[6] = 2 * (q.y * q.z + q.x * q.w);
                r.m[8] = 2 * (q.x * q.z + q.y * q.w);     r.m[9] = 2 * (q.y * q.z - q.x * q.w);     r.m[10] = 1 - 2 * (q.x * q.x + q.y * q.y);
                r.m[12] = bones[bi].bindLocal.m[12]; r.m[13] = bones[bi].bindLocal.m[13]; r.m[14] = bones[bi].bindLocal.m[14];
                times.push_back(k.time);
                std::istringstream is(rowMajor(f.transform(r))); float v; while (is >> v) mats.push_back(v);
            }
            const std::string id = c.name + "-" + t.bone;
            std::string interp; for (size_t i = 0; i < times.size(); ++i) interp += "LINEAR ";
            body += source(id + "-in", times, 1, "T") +
                          "<source id=\"" + id + "-out\"><float_array id=\"" + id + "-out-a\" count=\"" + std::to_string(mats.size()) + "\">" + floats(mats) +
                          "</float_array><technique_common><accessor source=\"#" + id + "-out-a\" count=\"" + std::to_string(times.size()) +
                          "\" stride=\"16\"><param name=\"TRANSFORM\" type=\"float4x4\"/></accessor></technique_common></source>"
                          "<source id=\"" + id + "-i\"><Name_array id=\"" + id + "-i-a\" count=\"" + std::to_string(times.size()) + "\">" + interp +
                          "</Name_array><technique_common><accessor source=\"#" + id + "-i-a\" count=\"" + std::to_string(times.size()) +
                          "\" stride=\"1\"><param name=\"INTERPOLATION\" type=\"name\"/></accessor></technique_common></source>"
                          "<sampler id=\"" + id + "-s\"><input semantic=\"INPUT\" source=\"#" + id + "-in\"/><input semantic=\"OUTPUT\" source=\"#" + id +
                          "-out\"/><input semantic=\"INTERPOLATION\" source=\"#" + id + "-i\"/></sampler><channel source=\"#" + id + "-s\" target=\"" +
                          t.bone + "/transform\"/>";
        }
        animations += "<animation id=\"" + c.name + "\" name=\"" + c.name + "\">" + body + "</animation>";
    }

    // Nodes: the scene's node tree, with the skeleton's bones as JOINT nodes under the root.
    std::function<std::string(size_t)> node = [&](size_t ni) {
        const Node& n = s.nodes[ni];
        std::string out = "<node id=\"" + n.name + "\" name=\"" + n.name + "\" sid=\"" + n.name + "\" type=\"NODE\"><matrix sid=\"transform\">" +
                          rowMajor(ni == 0 ? n.local : f.transform(n.local)) + "</matrix>";
        for (uint32_t mi : n.meshes) {
            const Mesh& m = s.meshes[mi];
            const std::string g = m.name + "-mesh";
            if (m.skinned())
                out += "<instance_controller url=\"#" + g + "-skin\"><skeleton>#" + s.skeleton->bones[0].name + "</skeleton>" + bindMaterials(m) + "</instance_controller>";
            else if (x.morph)
                out += "<instance_controller url=\"#" + g + "-morph\">" + bindMaterials(m) + "</instance_controller>";
            else
                out += "<instance_geometry url=\"#" + g + "\">" + bindMaterials(m) + "</instance_geometry>";
        }
        if (ni == 0 && s.skeleton) {
            std::function<std::string(int32_t)> joint = [&](int32_t bi) {
                const Bone& b = s.skeleton->bones[(size_t)bi];
                std::string j = "<node id=\"" + b.name + "\" name=\"" + b.name + "\" sid=\"" + b.name + "\" type=\"JOINT\"><matrix sid=\"transform\">" +
                                rowMajor(f.transform(b.bindLocal)) + "</matrix>";
                for (size_t k = 0; k < s.skeleton->bones.size(); ++k)
                    if (s.skeleton->bones[k].parent == bi) j += joint((int32_t)k);
                return j + "</node>";
            };
            for (size_t k = 0; k < s.skeleton->bones.size(); ++k) if (s.skeleton->bones[k].parent < 0) out += joint((int32_t)k);
        }
        if (ni == 0 && x.camera) out += "<node id=\"Cam\" name=\"Cam\" type=\"NODE\"><instance_camera url=\"#Cam-cam\"/></node>";
        for (size_t c = 0; c < s.nodes.size(); ++c) if (s.nodes[c].parent == (int32_t)ni) out += node(c);
        return out + "</node>";
    };
    if (!s.nodes.empty()) nodes = node(0);

    char unit[64]; std::snprintf(unit, sizeof unit, "%g", au.unit);
    return std::string("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<COLLADA xmlns=\"http://www.collada.org/2005/11/COLLADASchema\" version=\"1.4.1\">") +
           "<asset><unit name=\"u\" meter=\"" + unit + "\"/><up_axis>" + (au.zUp ? "Z_UP" : "Y_UP") + "</up_axis></asset>" +
           (x.camera ? "<library_cameras><camera id=\"Cam-cam\"><optics><technique_common><perspective><yfov>50</yfov><aspect_ratio>1</aspect_ratio><znear>0.1</znear><zfar>100</zfar></perspective></technique_common></optics></camera></library_cameras>" : "") +
           (effects.empty() ? "" : "<library_effects>" + effects + "</library_effects><library_materials>" + materials + "</library_materials>") +
           (geometries.empty() ? "" : "<library_geometries>" + geometries + "</library_geometries>") +
           (controllers.empty() ? "" : "<library_controllers>" + controllers + "</library_controllers>") +
           (animations.empty() ? "" : "<library_animations>" + animations + "</library_animations>") +
           "<library_visual_scenes><visual_scene id=\"Scene\" name=\"Scene\">" + nodes + "</visual_scene></library_visual_scenes>"
           "<scene><instance_visual_scene url=\"#Scene\"/></scene></COLLADA>\n";
}

}  // namespace dae

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("frontend_assimp_test\n");
    const fs::path dir = fs::temp_directory_path() / "wo013_frontend_assimp";
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto put = [&](const std::string& name, const std::string& text) { std::ofstream(dir / name) << text; return dir / name; };

    // ── 1. The contract suite, every case including the skinned ones ────────
    std::printf("1. the import-frontend contract suite\n");
    AssimpFrontend fe;
    impcontract::Subject subj{"assimp", &fe, [&](Case c) -> std::optional<fs::path> {
        const std::string name = std::string(impcontract::name(c)) + ".dae";
        switch (c) {
            case Case::AuthoredCentimetreZUp:
                return put(name, dae::write(impcontract::expected(c), {.unit = 0.01f, .zUp = true}));
            case Case::Unrepresentable:
                // Skipped, with the reason: the vendored Assimp cannot read a COLLADA
                // <morph> at all (ColladaParser.cpp's <targets> loop walks the
                // controller's children, not the targets', so every morph file fails
                // to import), and no other text format it reads carries morphs.
                // Vertex colours and cameras are checked on their own in §2.
                return std::nullopt;
            case Case::EmptyFile: {                    // a file that parses and holds one empty node
                ImportedScene empty;
                Node n; n.name = "empty";
                empty.nodes = {n};
                return put(name, dae::write(empty));
            }
            default:
                return put(name, dae::write(impcontract::expected(c)));
        }
    }};
    const impcontract::Report r = impcontract::run(subj);
    for (const auto& f : r.failures) std::printf("        %s\n", f.c_str());
    if (r.failedCase(Case::SkinnedColumn))                   // show what was read, when it is wrong
        if (const ImportResult got = fe.importScene(dir / "SkinnedColumn.dae", {}); got) {
            for (const Clip& c : got.scene().clips) std::printf("        read clip '%s' (%.3f s, %zu tracks)\n", c.name.c_str(), c.duration, c.tracks.size());
            if (got.scene().skeleton) for (const Bone& b : got.scene().skeleton->bones) std::printf("        read bone '%s' (parent %d)\n", b.name.c_str(), b.parent);
        }
    CHECK(r.failures.empty(), "no case fails (%d passed)", r.passed);
    CHECK(r.skipped == std::vector<std::string>({"Unrepresentable"}),
          "only Unrepresentable is skipped: Assimp cannot read a COLLADA morph (%zu skipped)", r.skipped.size());

    // ── 2. The losses COLLADA can carry, reported with the right effect ─────
    std::printf("2. the dropped list\n");
    {
        const ImportResult t = fe.importScene(put("colours_camera.dae",
            dae::write(impcontract::expected(Case::UnitTriangle), {}, {.colours = true, .camera = true})), {});
        bool colours = false, camera = false;
        if (t) for (const Dropped& d : t.scene().dropped) {
            colours |= d.kind == Dropped::Kind::VertexColours && d.effect == Dropped::Effect::Less;
            camera  |= d.kind == Dropped::Kind::Camera && d.effect == Dropped::Effect::Less;
        }
        CHECK(t && colours && camera, "vertex colours and a camera are each dropped as Less, and the triangle still imports");
    }

    // ── 2b. A DCC's junk clip name becomes the file's stem ──────────────────
    // Mixamo and most DCC exports name every take "Take 001" or "mixamo.com";
    // the old paths named such a clip after the file, and so does this.
    {
        ImportedScene walk = impcontract::expected(Case::SkinnedColumn);
        walk.clips[0].name = "Take 001";
        const ImportResult t = fe.importScene(put("Walk.dae", dae::write(walk)), {});
        CHECK(t && t.scene().clips.size() == 1 && t.scene().clips[0].name == "Walk",
              "a clip named 'Take 001' in Walk.dae is named 'Walk' (%s)", t && !t.scene().clips.empty() ? t.scene().clips[0].name.c_str() : "-");
    }

    // ── 3. Texture paths: as written, else the basename beside the source ───
    // FBX files routinely carry an absolute path from the author's machine
    // (Medieval Village's props name C:\Users\...\Textures\*.png). The one
    // fallback is that file's name beside the source; nothing else is searched.
    std::printf("3. texture references\n");
    {
        { std::ofstream(dir / "wood.tga", std::ios::binary) << std::string(18 + 64, '\0'); }
        { std::ofstream(dir / "prop.mtl") << "newmtl Wood\nmap_Kd C:/Users/author/Textures/wood.tga\n"
                                           << "newmtl Stone\nmap_Kd C:/Users/author/Textures/stone.tga\n"; }
        { std::ofstream(dir / "prop.obj") << "mtllib prop.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nv 1 1 0\nvt 0 0\n"
                                           << "usemtl Wood\nf 1/1 2/1 3/1\nusemtl Stone\nf 2/1 4/1 3/1\n"; }
        const ImportResult t = fe.importScene(dir / "prop.obj", {});
        const Material* wood = nullptr; const Material* stone = nullptr;
        if (t) for (const Material& m : t.scene().materials) { if (m.name == "Wood") wood = &m; if (m.name == "Stone") stone = &m; }
        bool dropped = false;
        if (t) for (const Dropped& d : t.scene().dropped) dropped |= d.kind == Dropped::Kind::Texture && d.what.find("stone.tga") != std::string::npos;
        CHECK(wood && wood->baseColor.path == "wood.tga",
              "an author's absolute path resolves to the file of that name beside the source (%s)", wood ? wood->baseColor.path.c_str() : "-");
        CHECK(stone && stone->baseColor.empty() && dropped, "and one found nowhere is dropped, naming it");
    }

    // ── 4. A node tree 10,000 deep ──────────────────────────────────────────
    // The front end walked Assimp's node tree by recursion, and so did the
    // skeleton extraction under it; a deep enough file overflowed a 512 KB
    // stack (a macOS secondary thread, where cooks run) with SIGBUS, which no
    // exception boundary catches. The chain wraps the whole visual scene, so in
    // the skinned case it is every joint's ancestor. Built by string surgery on
    // the written file: dae::write nests by concatenation and is quadratic.
    std::printf("4. deep node trees\n");
    {
        constexpr int kDepth = 10000;
        for (Case c : {Case::UnitTriangle, Case::SkinnedColumn}) {
            std::string x = dae::write(impcontract::expected(c));
            const size_t open = x.find('>', x.find("<visual_scene")) + 1, close = x.find("</visual_scene>");
            std::string head, tail;
            head.reserve(kDepth * 48); tail.reserve(kDepth * 8);
            for (int i = 0; i < kDepth; ++i) {
                head += "<node id=\"chain" + std::to_string(i) + "\" name=\"chain" + std::to_string(i) + "\" type=\"NODE\">";
                tail += "</node>";
            }
            x = x.substr(0, open) + head + x.substr(open, close - open) + tail + x.substr(close);
            const fs::path p = put(std::string("deep_") + impcontract::name(c) + ".dae", x);
            bool read = false, valid = false; size_t nodes = 0; std::string why;
            const bool ran = engine::threads::runWithStack(512 * 1024, [&] {
                const ImportResult d = fe.importScene(p, {});
                read = (bool)d;
                if (!d) why = d.error().message;
                else { nodes = d.scene().nodes.size(); valid = checkScene(d.scene()).empty(); }
            });
            CHECK(ran && read && valid && nodes > (size_t)kDepth,
                  "%s under a %d-deep node chain imports on a 512 KB stack, and the scene is valid (%zu nodes%s%s)",
                  impcontract::name(c), kDepth, nodes, why.empty() ? "" : "; ", why.c_str());
        }
    }

    // ── 5. A rig over the engine's bone limit is read WHOLE ─────────────────
    // extractSkeleton kept the first 128 bones and dropped the rest with a line
    // on stderr, and extractBoneWeights dropped every influence on a bone past
    // the 256th. So a 300-bone rig imported as a different, smaller rig, and
    // its top vertices lost their bone. The front end carries the file; the
    // limit is the cook's to enforce, by name (mesh_backend_test, WO-040).
    std::printf("5. a 300-bone rig\n");
    {
        const ImportedScene want = impcontract::build::rig(300);
        const ImportResult t = fe.importScene(put("rig300.dae", dae::write(want)), {});
        std::vector<std::string> why;
        if (t) { impcontract::detail::compareGeometry(t.scene(), want, why); impcontract::detail::compareSkeleton(t.scene(), want, why); }
        CHECK(t && t.scene().skeleton && t.scene().skeleton->bones.size() >= 300 && why.empty(),
              "every bone is read and the top vertices stay on 'Bone299' (%zu bones%s%s)",
              t && t.scene().skeleton ? t.scene().skeleton->bones.size() : (size_t)0,
              why.empty() ? "" : "; ", why.empty() ? "" : why[0].c_str());
    }

    fs::remove_all(dir);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
