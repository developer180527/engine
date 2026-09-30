// ── fuzz_import_frontend_test — a source model file is untrusted input ──────
//
// The import front ends (cgltf, Assimp) are the first code to read a model
// file an artist, a download or a marketplace hands the engine, and the one
// back end cooks whatever they return. Before this target, fuzzing stopped at
// the COOKED format (fuzz_mesh_loader_test, fuzz_cooked_skin_test): the parsers
// of outside files were covered only by the hand-written contract cases. A
// skin with "joints": [] crashed the cook worker, and nothing would have found
// it (see frontend_cgltf_test §4).
//
// Inputs start from the contract suite's reference scenes, written as real
// files (glTF through tests/gltf_writer.h, OBJ as text), so most mutations land
// on structure a parser actually walks instead of failing at byte one.
//
// Properties, per case:
//   1. The unmutated file imports (the generator is sound).
//   2. A mutated file NEVER crashes and never lets an exception escape: the
//      answer is a scene or an ImportError.
//   3. Whatever a front end returns, the back end refuses or cooks without
//      crashing; a cook it reports as successful loads back.
//   4. A scene the front end returns passes checkScene, or the back end refuses
//      it. The contract suite requires valid scenes of the reference cases;
//      for hostile input the requirement is only that nothing invalid COOKS.
#include <algorithm>
#include <cstdio>
#include <exception>
#include <map>
#include <fstream>
#include <string>
#include <vector>

#include <assetlib/mesh_asset.h>
#include <nlohmann/json.hpp>

#include "assets/cookers/mesh/mesh_backend.h"
#include "assets/import/frontend_assimp.h"
#include "assets/import/frontend_cgltf.h"
#include "assets/import/imported_scene_check.h"
#include "fuzz/fuzz.h"
#include "gltf_writer.h"
#include "import_contract.h"

namespace fs = std::filesystem;
using impcontract::Case;

namespace {

constexpr uint32_t kGeneratorVersion = 3;   // 3: some glTF cases hang under a node chain thousands deep
                                            // 2: glTF mutations edit the JSON tree (most byte edits never parsed)

// The reference cases a format can write. EmptyFile is written as an empty
// scene; Unrepresentable is left out, as its point (morphs) is not structure.
constexpr Case kBases[] = {Case::UnitTriangle, Case::NodeTransforms, Case::TwoMaterialQuad,
                           Case::SkinnedColumn, Case::AuthoredCentimetreZUp};

// An OBJ of the case's first mesh: positions, UVs, normals, faces. Enough
// grammar for Assimp's OBJ reader to walk every statement kind it cares about.
std::string writeObj(const imp::ImportedScene& s) {
    std::string o = "# fuzz\nmtllib none.mtl\no mesh\n";
    char line[160];
    if (s.meshes.empty()) return o;
    const imp::Mesh& m = s.meshes[0];
    for (const auto& p : m.positions) { std::snprintf(line, sizeof line, "v %g %g %g\n", p.x, p.y, p.z); o += line; }
    for (const auto& t : m.uv0)       { std::snprintf(line, sizeof line, "vt %g %g\n", t.x, t.y); o += line; }
    for (const auto& n : m.normals)   { std::snprintf(line, sizeof line, "vn %g %g %g\n", n.x, n.y, n.z); o += line; }
    o += "usemtl a\n";
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        const unsigned a = m.indices[i] + 1, b = m.indices[i + 1] + 1, c = m.indices[i + 2] + 1;
        std::snprintf(line, sizeof line, "f %u/%u/%u %u/%u/%u %u/%u/%u\n", a, a, a, b, b, b, c, c, c);
        o += line;
    }
    return o;
}

// Splices aimed at what the readers index with: counts, indices, references
// between objects, skins, animation samplers, and nesting depth. Never a "uri":
// an external buffer is read from disk, and a fuzzer must not make the cook
// read arbitrary files.
const char* const kGltfSplices[] = {
    "\"joints\":[],", "\"joints\":[0,0,0],", "\"skin\":7,", "\"skin\":0,", "\"mesh\":9,",
    "\"count\":4294967295,", "\"count\":0,", "\"count\":-1,", "\"byteOffset\":999999,",
    "\"byteStride\":3,", "\"componentType\":5126,", "\"componentType\":5121,",
    "\"type\":\"MAT4\",", "\"type\":\"SCALAR\",", "\"indices\":99,", "\"material\":-2,",
    "\"children\":[0],", "\"nodes\":[0,0],", "\"inverseBindMatrices\":0,",
    "\"sampler\":5,", "\"interpolation\":\"CUBICSPLINE\",", "\"path\":\"weights\",",
    "\"input\":0,", "\"output\":0,", "\"node\":99,", "\"scale\":[0,0,0],",
    "\"rotation\":[0,0,0,0],", "\"matrix\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],",
    "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[", "1e39", "nan", "-0", "\"\\u0000\"",
};
const char* const kObjSplices[] = {
    "f 1 2 999999\n", "f -1 -2 -3\n", "f 0 0 0\n", "f 1/1/1\n", "f 1 2 3 4 5 6 7 8 9\n",
    "v nan nan nan\n", "v 1e39 0 0\n", "vt\n", "vn 0 0\n", "usemtl\n", "o\n", "g\n",
    "s off\n", "l 1 2\n", "p 1\n", "f 1//\n", "f //1\n", "\\\n", "\x00\x01\xff\n",
};

template <size_t N>
void mutate(fuzz::Rng& rng, std::string& s, const char* const (&splices)[N]) {
    const int edits = (int)rng.range(1, 5);
    for (int e = 0; e < edits && !s.empty(); ++e) {
        const size_t at = rng.range(0, (uint32_t)s.size() - 1);
        switch (rng.range(0, 4)) {
        case 0: s[at] = (char)rng.range(0, 255); break;                           // flip a byte
        case 1: s.resize(at); break;                                              // truncate
        case 2: s.insert(at, splices[rng.below((uint32_t)N)]); break;             // hostile token
        case 3: s.erase(at, rng.range(1, 24)); break;                             // delete a run
        default: {                                                                // a digit, pushed to an edge
            const size_t d = s.find_first_of("0123456789", at);
            if (d != std::string::npos)
                s.replace(d, 1, std::to_string(rng.interestingU32()));
        }
        }
    }
}

// A glTF mutation that keeps the file VALID JSON: pick a node of the tree and
// change it the way a broken exporter would — a number pushed to an edge, an
// array element dropped or duplicated, a key removed, an object's value swapped
// for another type. Byte edits almost always stop at the JSON parser; these
// reach cgltf's semantic checks, the reader and the back end.
// Integers where readers break (0, -1, the 8/16/32-bit edges) and glTF's own
// enum values (component types, buffer targets), so a swapped one is plausible.
nlohmann::json kNumbersEdge(fuzz::Rng& rng) {
    static const int64_t kInts[] = {0, -1, 1, 2, 3, 4, 255, 256, 65535, 65536, 4294967295LL, -2147483648LL,
                                    5120, 5121, 5122, 5123, 5125, 5126, 34962, 34963};
    if (rng.chance(15)) return rng.chance(50) ? 1e39 : 0.5;
    return kInts[rng.below((uint32_t)std::size(kInts))];
}

void mutateTree(fuzz::Rng& rng, nlohmann::json& root) {
    std::vector<nlohmann::json*> nodes;
    std::vector<nlohmann::json*> stack{&root};
    while (!stack.empty()) {
        nlohmann::json* n = stack.back(); stack.pop_back();
        // The embedded buffer is a data: URI; leave it whole so accessors resolve.
        if (n->is_string() && n->get_ref<const std::string&>().rfind("data:", 0) == 0) continue;
        nodes.push_back(n);
        if (n->is_object()) for (auto& [k, v] : n->items()) stack.push_back(&v);
        if (n->is_array())  for (auto& v : *n) stack.push_back(&v);
    }
    if (nodes.empty()) return;
    // Mostly numbers: the counts, offsets, indices and references a reader
    // trusts. cgltf refuses a value of the wrong TYPE at parse time, which is
    // correct and tests nothing past the front door, so type changes are rare.
    std::vector<nlohmann::json*> numbers;
    for (nlohmann::json* x : nodes) if (x->is_number()) numbers.push_back(x);
    const bool number = !numbers.empty() && rng.chance(80);
    nlohmann::json& n = number ? *numbers[rng.below((uint32_t)numbers.size())]
                               : *nodes[rng.below((uint32_t)nodes.size())];
    if (number) {
        if (rng.chance(50)) n = kNumbersEdge(rng);
        else if (n.is_number_integer()) n = n.get<int64_t>() + (int64_t)rng.range(0, 4) - 2 + (rng.chance(20) ? 100 : 0);
        else n = n.get<double>() * (rng.chance(50) ? -1.0 : 1000.0);
        return;
    }
    switch (rng.range(0, 5)) {
    case 0:   // anything becomes an edge number
        n = kNumbersEdge(rng);
        break;
    case 1:   // an array loses or gains an element
        if (n.is_array() && !n.empty()) {
            if (rng.chance(50)) n.erase(n.begin() + rng.below((uint32_t)n.size()));
            else                n.push_back(n[rng.below((uint32_t)n.size())]);
        } else n = nlohmann::json::array();
        break;
    case 2:   // an object loses a key
        if (n.is_object() && !n.empty()) {
            auto it = n.begin(); std::advance(it, rng.below((uint32_t)n.size())); n.erase(it);
        } else n = nlohmann::json::object();
        break;
    case 3:   // a value of the wrong type
        switch (rng.range(0, 3)) {
        case 0: n = "SCALAR"; break;
        case 1: n = nullptr; break;
        case 2: n = true; break;
        default: n = nlohmann::json::array({0, 0, 0});
        }
        break;
    case 4:   // an empty joints list, the crash this target was written after
        if (n.is_object()) n["joints"] = nlohmann::json::array(); else n = 0;
        break;
    default:  // an index that points at the next object over, or off the end
        if (n.is_number_integer()) n = n.get<int64_t>() + (rng.chance(50) ? 1 : 100);
        else n = 1;
    }
}

struct Stats { uint64_t imported = 0, refusedImport = 0, invalidScenes = 0, cooked = 0, refusedCook = 0; };
Stats g_stats;
std::map<std::string, uint64_t> g_refusals;   // why imports were refused, by reason (path stripped)

// Properties 2-4 for one file on disk.
void exercise(const imp::IImportFrontend& fe, const fs::path& file, const fs::path& out,
              const fuzz::ReproKey& key, fuzz::Report& rep) {
    imp::ImportResult r = imp::ImportError{};
    try {
        r = fe.importScene(file, {});
    } catch (const std::exception& e) {
        rep.fail(key, std::string(fe.name()) + ": an exception escaped importScene: " + e.what());
        return;
    } catch (...) {
        rep.fail(key, std::string(fe.name()) + ": a non-std exception escaped importScene");
        return;
    }
    if (!r) {
        ++g_stats.refusedImport;
        std::string why = r.error().message;
        if (const size_t cut = why.find(": /"); cut != std::string::npos) why.resize(cut);   // drop the scratch path
        if (const size_t cut = why.rfind(file.string()); cut != std::string::npos) why.resize(cut);
        ++g_refusals[std::string(fe.name()) + ": " + why.substr(0, 60)];
        return;
    }
    ++g_stats.imported;

    const bool valid = imp::checkScene(r.scene()).empty();
    if (!valid) ++g_stats.invalidScenes;

    assetlib::CookContext ctx;
    ctx.sourcePath = file;
    ctx.outputPath = out;
    std::error_code ec;
    fs::remove(out, ec);
    assetlib::CookResult c;
    try {
        c = meshcook::cookImportedScene(r.scene(), ctx);
    } catch (const std::exception& e) {
        rep.fail(key, std::string(fe.name()) + ": the back end threw: " + e.what());
        return;
    }
    if (!c.success) { ++g_stats.refusedCook; return; }
    ++g_stats.cooked;
    if (!valid)
        rep.fail(key, std::string(fe.name()) + ": the back end cooked a scene that fails checkScene");
    assetlib::MeshAsset back;
    if (!assetlib::loadMesh(back, out))
        rep.fail(key, std::string(fe.name()) + ": a cook reported successful does not load back");
}

const fuzz::Scratch& scratch() {
    static const fuzz::Scratch s("import_frontend");
    return s;
}

void oneCase(uint64_t masterSeed, fuzz::Report& rep) {
    fuzz::ReproKey key;
    key.masterSeed = masterSeed;
    key.generatorVersion = kGeneratorVersion;
    key.target = "import_frontend";

    fuzz::Rng pick(fuzz::deriveSeed(masterSeed, "import_base"));
    fuzz::Rng mut(fuzz::deriveSeed(masterSeed, "import_mutation"));
    const Case base = kBases[pick.below((uint32_t)std::size(kBases))];
    const imp::ImportedScene want = impcontract::expected(base);
    const fs::path dir = scratch().path();

    static const imp::CgltfFrontend  gltf;
    static const imp::AssimpFrontend assimp;
    const bool useGltf = pick.chance(70);   // glTF carries skins, clips and hierarchy; OBJ only geometry

    const std::string clean = useGltf ? gltfw::write(want) : writeObj(want);
    const fs::path file = dir / (useGltf ? "case.gltf" : "case.obj");
    const fs::path out  = dir / "case.cooked";
    auto put = [&](const std::string& text) { std::ofstream(file, std::ios::binary) << text; };

    // 1. The generator is sound.
    put(clean);
    {
        const imp::ImportResult r = (useGltf ? static_cast<const imp::IImportFrontend&>(gltf) : assimp).importScene(file, {});
        if (!r) rep.fail(key, std::string("an unmutated ") + impcontract::name(base) + (useGltf ? " .gltf" : " .obj") +
                              " was refused: " + r.error().message);
    }

    // 2-4. Mutations.
    // Depth is a resource too: both front ends walked the node tree by
    // recursion, and a 2,000-deep chain overflowed a 512 KB stack. One glTF
    // case in ten hangs its scene under a chain up to 20,000 deep, and then
    // mutates as usual, so the depth reaches the skeleton and the back end too.
    // Its own stream: the mutations drawn from `mut` are what they were.
    fuzz::Rng deepRng(fuzz::deriveSeed(masterSeed, "import_depth"));
    std::string deepClean = clean;
    if (useGltf && deepRng.chance(10)) {
        nlohmann::json j = nlohmann::json::parse(clean);
        auto& nodes = j["nodes"];
        const nlohmann::json roots = j["scenes"][0]["nodes"];
        const int first = (int)nodes.size(), depth = (int)deepRng.range(500, 20000);
        for (int i = 0; i < depth; ++i)
            nodes.push_back({{"children", i + 1 < depth ? nlohmann::json::array({first + i + 1}) : roots}});
        j["scenes"][0]["nodes"] = nlohmann::json::array({first});
        deepClean = j.dump();
    }
    for (int round = 0; round < 6; ++round) {
        std::string m = deepClean;
        if (useGltf && mut.chance(75)) {
            nlohmann::json j = nlohmann::json::parse(m, nullptr, false);
            const int edits = (int)mut.range(1, 3);
            for (int e = 0; e < edits; ++e) mutateTree(mut, j);
            m = j.dump();
        } else if (useGltf) mutate(mut, m, kGltfSplices);
        else         mutate(mut, m, kObjSplices);
        put(m);
        exercise(useGltf ? static_cast<const imp::IImportFrontend&>(gltf) : assimp, file, out, key, rep);
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const int rc = fuzz::run("import_frontend", argc, argv, oneCase);
    // What the mutations reached, so a run that only ever hits "unreadable"
    // (and so tests nothing past the parser's front door) is visible.
    std::printf("[reach] imported %llu (invalid %llu), refused at import %llu; cooked %llu, refused at cook %llu\n",
                (unsigned long long)g_stats.imported, (unsigned long long)g_stats.invalidScenes,
                (unsigned long long)g_stats.refusedImport, (unsigned long long)g_stats.cooked,
                (unsigned long long)g_stats.refusedCook);
    std::vector<std::pair<uint64_t, std::string>> top;
    for (const auto& [why, n] : g_refusals) top.push_back({n, why});
    std::sort(top.rbegin(), top.rend());
    for (size_t i = 0; i < top.size() && i < 6; ++i)
        std::printf("        %6llu  %s\n", (unsigned long long)top[i].first, top[i].second.c_str());
    return rc;
}
