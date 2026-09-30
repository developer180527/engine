// ── frontend_cgltf_test — the cgltf front end against the contract suite (WO-012)
//
// Each reference case of tests/import_contract.h is WRITTEN as a real .gltf
// file (JSON with an embedded base64 buffer) from the case's own expected
// scene, then imported by CgltfFrontend and compared by meaning. So the file
// the front end reads says exactly what the suite expects back, in glTF.
//
// Skipped until WO-014, and reported as such: the two skinned cases. glTF can
// express them; this front end does not read skins yet (it drops them as Wrong,
// which is WO-002's refusal, now in the dropped list).
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
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (size_t i = 0; i < count; ++i)
                for (int k = 0; k < 3; ++k) { lo[k] = std::min(lo[k], f[i * 3 + k]); hi[k] = std::max(hi[k], f[i * 3 + k]); }
            char b[256]; std::snprintf(b, sizeof b, ",\"min\":[%g,%g,%g],\"max\":[%g,%g,%g]", lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]);
            mm = b;
        }
        accessors += (nAccessors ? "," : "") + std::string("{\"bufferView\":") + std::to_string(nViews) +
                     ",\"componentType\":5126,\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"" + mm + "}";
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

struct Extras { bool morph = false, colours = false, camera = false; };

// A static ImportedScene as glTF: one glTF mesh per imp::Mesh, one primitive
// per submesh (sharing the vertex accessors), nodes with explicit matrices.
std::string write(const ImportedScene& s, Extras x = {}) {
    Buffer b;
    std::string meshes, materials, nodes;
    for (size_t mi = 0; mi < s.meshes.size(); ++mi) {
        const Mesh& m = s.meshes[mi];
        const int pos = b.floats(&m.positions[0].x, m.positions.size(), 3, "VEC3", true);
        const int nrm = b.floats(&m.normals[0].x, m.normals.size(), 3, "VEC3");
        const int uv  = m.uv0.empty() ? -1 : b.floats(&m.uv0[0].x, m.uv0.size(), 2, "VEC2");
        int col = -1, morph = -1;
        if (x.colours) { std::vector<float> c(m.positions.size() * 4, 1.0f); col = b.floats(c.data(), m.positions.size(), 4, "VEC4"); }
        if (x.morph)   { std::vector<float> d(m.positions.size() * 3, 0.0f); morph = b.floats(d.data(), m.positions.size(), 3, "VEC3", true); }
        std::string prims;
        for (size_t si = 0; si < m.submeshes.size(); ++si) {
            const Submesh& sm = m.submeshes[si];
            const int ix = b.indices(&m.indices[sm.firstIndex], sm.indexCount);
            prims += (si ? "," : "") + std::string("{\"attributes\":{\"POSITION\":") + std::to_string(pos) +
                     ",\"NORMAL\":" + std::to_string(nrm) + (uv >= 0 ? ",\"TEXCOORD_0\":" + std::to_string(uv) : "") +
                     (col >= 0 ? ",\"COLOR_0\":" + std::to_string(col) : "") + "},\"indices\":" + std::to_string(ix) +
                     ",\"material\":" + std::to_string(sm.material) +
                     (morph >= 0 ? ",\"targets\":[{\"POSITION\":" + std::to_string(morph) + "},{\"POSITION\":" + std::to_string(morph) + "}]" : "") + "}";
        }
        meshes += (mi ? "," : "") + std::string("{\"name\":\"") + m.name + "\",\"primitives\":[" + prims + "]}";
    }
    for (size_t i = 0; i < s.materials.size(); ++i) {
        const Float4 c = s.materials[i].baseColorFactor;
        char f[160]; std::snprintf(f, sizeof f, "[%g,%g,%g,%g]", c.x, c.y, c.z, c.w);
        materials += (i ? "," : "") + std::string("{\"name\":\"") + s.materials[i].name +
                     "\",\"pbrMetallicRoughness\":{\"baseColorFactor\":" + f + ",\"roughnessFactor\":0.7,\"metallicFactor\":0}}";
    }
    for (size_t ni = 0; ni < s.nodes.size(); ++ni) {
        const Node& n = s.nodes[ni];
        std::string mat = "[";
        for (int k = 0; k < 16; ++k) { char f[32]; std::snprintf(f, sizeof f, "%s%.9g", k ? "," : "", n.local.m[k]); mat += f; }
        mat += "]";
        std::string kids;
        for (size_t c = 0; c < s.nodes.size(); ++c)
            if (s.nodes[c].parent == (int32_t)ni) kids += (kids.empty() ? "" : ",") + std::to_string(c);
        if (ni == 0 && x.camera) kids += (kids.empty() ? "" : ",") + std::to_string(s.nodes.size());
        nodes += (ni ? "," : "") + std::string("{\"name\":\"") + n.name + "\",\"matrix\":" + mat +
                 (n.meshes.empty() ? "" : ",\"mesh\":" + std::to_string(n.meshes[0])) +
                 (kids.empty() ? "" : ",\"children\":[" + kids + "]") + "}";
    }
    if (x.camera) nodes += ",{\"name\":\"Cam\",\"camera\":0}";
    while (b.data.size() % 4) b.data.push_back(0);
    std::string j = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],\"nodes\":[" + nodes + "]";
    if (!meshes.empty()) j += ",\"meshes\":[" + meshes + "]";
    if (!materials.empty()) j += ",\"materials\":[" + materials + "]";
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
            case Case::SkinnedColumn:
            case Case::AuthoredCentimetreZUp:  return std::nullopt;        // skins: WO-014
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
    CHECK(r.skipped == std::vector<std::string>({"SkinnedColumn", "AuthoredCentimetreZUp"}),
          "exactly the two skinned cases are skipped, pending WO-014 (%zu skipped)", r.skipped.size());

    // ── 2. What the front end drops, and with what effect ───────────────────
    std::printf("2. the dropped list\n");
    {
        // WO-002's refusal, now in the dropped list: a skin is Wrong.
        // The unit triangle, its node given a skin whose one joint is a second node.
        std::string skinned = gltfw::write(impcontract::expected(Case::UnitTriangle));
        skinned.replace(skinned.find("\"name\":\"root\""), std::strlen("\"name\":\"root\""), "\"name\":\"root\",\"skin\":0");
        skinned.replace(skinned.find("\"scenes\":[{\"nodes\":[0]}]"), std::strlen("\"scenes\":[{\"nodes\":[0]}]"),
                        "\"scenes\":[{\"nodes\":[0,1]}],\"skins\":[{\"joints\":[1]}]");
        skinned.insert(skinned.find("],\"meshes\""), ",{\"name\":\"bone\"}");
        const ImportResult s = fe.importScene(put("skinned.gltf", skinned), {});
        bool wrongSkin = false;
        if (s) for (const auto& d : s.scene().dropped) wrongSkin |= d.kind == Dropped::Kind::Skin && d.effect == Dropped::Effect::Wrong;
        CHECK(s && wrongSkin, "a skin is dropped as Wrong, so the back end refuses the cook (WO-002)");

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
