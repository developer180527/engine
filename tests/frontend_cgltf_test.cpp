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

#include <nlohmann/json.hpp>

#include "assets/import/frontend_cgltf.h"
#include "gltf_writer.h"   // gltfw::write
#include "import_contract.h"
#include "core/thread_stack.h"   // engine::threads::runWithStack

static int g_failures = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("  FAIL  " __VA_ARGS__); std::printf("\n"); ++g_failures; } \
                           else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } } while (0)

namespace fs = std::filesystem;
using namespace imp;
using impcontract::Case;


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

    // ── 4. Malformed files are refused or read, never a crash ───────────────
    std::printf("4. malformed files\n");
    {
        // A skin with "joints": [] on a skinned mesh. cgltf accepts it and
        // cgltf_validate does not object. Every weight then names a joint past
        // the end, the all-zero fallback bound the vertex to joints[0] of an
        // EMPTY array, and the cook worker read through a null pointer. The
        // other skin keeps the file's skeleton alive, as it must for the path
        // to be reached.
        std::string j = gltfw::write(impcontract::expected(Case::SkinnedColumn));
        const size_t at = j.find("\"skins\":[");
        const bool edited = at != std::string::npos;
        if (edited) j.insert(at + std::strlen("\"skins\":["), "{\"joints\":[]},");   // the mesh's skin 0 is now empty
        const ImportResult e = fe.importScene(put("empty_skin.gltf", j), {});
        bool wrongSkin = false;
        if (e) for (const auto& d : e.scene().dropped)
            wrongSkin |= d.kind == Dropped::Kind::Skin && d.effect == Dropped::Effect::Wrong;
        CHECK(edited && e && !e.scene().meshes.empty() && !e.scene().meshes[0].skinned() && wrongSkin,
              "a mesh on a skin with no joints: read static, no crash, and dropped as Skin/Wrong "
              "(the back end refuses it, as it would any skinned mesh without its skeleton)");
    }

    {
        // inverseBindMatrices shorter than the joint list. cgltf_validate does
        // not compare them and cgltf_accessor_read_float does not bounds-check,
        // so reading one matrix per joint overflowed the buffer (ASan, found by
        // fuzz_import_frontend_test). Now: Skin/Wrong, nothing read past the end.
        nlohmann::json j = nlohmann::json::parse(gltfw::write(impcontract::expected(Case::SkinnedColumn)));
        const int ibm = j["skins"][0]["inverseBindMatrices"].get<int>();
        j["accessors"][ibm]["count"] = 1;
        const ImportResult s = fe.importScene(put("short_ibm.gltf", j.dump()), {});
        bool wrong = false;
        if (s) for (const auto& d : s.scene().dropped)
            wrong |= d.kind == Dropped::Kind::Skin && d.effect == Dropped::Effect::Wrong &&
                     d.what.find("inverseBindMatrices") != std::string::npos;
        CHECK(s && wrong, "inverseBindMatrices with fewer matrices than joints: Skin/Wrong, not a read past the buffer");

        // An accessor off its component alignment (the spec requires it;
        // cgltf_validate does not check, and cgltf's typed reads are then UB).
        // The buffer grows by 8 bytes so the shifted accessor still fits its
        // view: otherwise cgltf_validate refuses it for size, and the test would
        // pass without ever reaching the alignment check.
        nlohmann::json a = nlohmann::json::parse(gltfw::write(impcontract::expected(Case::UnitTriangle)));
        std::string& uri = a["buffers"][0]["uri"].get_ref<std::string&>();
        const std::string prefix = uri.substr(0, uri.find(',') + 1);
        std::vector<uint8_t> bytes;
        {   // decode, append 8 zero bytes, re-encode
            const std::string b64 = uri.substr(prefix.size());
            auto val = [](char c) -> int {
                if (c >= 'A' && c <= 'Z') return c - 'A';
                if (c >= 'a' && c <= 'z') return c - 'a' + 26;
                if (c >= '0' && c <= '9') return c - '0' + 52;
                return c == '+' ? 62 : c == '/' ? 63 : -1;
            };
            uint32_t acc = 0; int bits = 0;
            for (char c : b64) {
                const int v = val(c); if (v < 0) continue;
                acc = (acc << 6) | (uint32_t)v; bits += 6;
                if (bits >= 8) { bits -= 8; bytes.push_back((uint8_t)(acc >> bits)); }
            }
        }
        bytes.resize(bytes.size() + 8, 0);
        uri = prefix + gltfw::base64(bytes);
        a["buffers"][0]["byteLength"] = bytes.size();
        const int view = a["accessors"][0]["bufferView"].get<int>();
        a["bufferViews"][view]["byteLength"] = a["bufferViews"][view]["byteLength"].get<int>() + 8;
        a["accessors"][0]["byteOffset"] = a["accessors"][0].value("byteOffset", 0) + 1;
        const ImportResult m = fe.importScene(put("shifted.gltf", a.dump()), {});
        CHECK(!m && m.error().message.find("is not aligned") != std::string::npos,
              "a misaligned accessor is refused as invalid glTF, naming the alignment%s",
              m ? "" : (" (" + m.error().message.substr(0, 60) + ")").c_str());
    }
    {
        // Rotation keys a little off unit length, as exporters write them after
        // quantisation: normalised on read, so the clip is a valid one.
        std::string j = gltfw::write(impcontract::expected(Case::SkinnedColumn));
        ImportedScene want = impcontract::expected(Case::SkinnedColumn);
        for (auto& k : want.clips[0].tracks[0].rotation)
            k.value = {k.value.x * 1.01f, k.value.y * 1.01f, k.value.z * 1.01f, k.value.w * 1.01f};
        j = gltfw::write(want);
        const ImportResult n = fe.importScene(put("off_unit.gltf", j), {});
        bool unit = n && !n.scene().clips.empty();
        if (unit) for (const auto& tr : n.scene().clips[0].tracks)
            for (const auto& k : tr.rotation) {
                const float n2 = k.value.x * k.value.x + k.value.y * k.value.y + k.value.z * k.value.z + k.value.w * k.value.w;
                unit &= std::fabs(n2 - 1.0f) < 1e-4f;
            }
        CHECK(unit && checkScene(n.scene()).empty(), "rotation keys 1%% off unit length are normalised, and the scene is valid");
    }

    {
        // A node tree 10,000 deep. Walking it by recursion overflowed a 512 KB
        // stack (the size of a macOS secondary thread, where cooks run) from
        // about 2,000 levels: SIGBUS, which no exception boundary catches. The
        // chain sits ABOVE the scene's own nodes, so it is every bone's ancestor
        // in the skinned case too, and also exercises the skeleton's ancestor
        // walk. Run on a stack of known size, so macOS and Linux agree.
        constexpr int kDepth = 10000;
        auto deepen = [&](Case c) {
            nlohmann::json j = nlohmann::json::parse(gltfw::write(impcontract::expected(c)));
            auto& nodes = j["nodes"];
            const nlohmann::json roots = j["scenes"][0]["nodes"];
            const int first = (int)nodes.size();
            for (int i = 0; i < kDepth; ++i) {
                nlohmann::json n = {{"name", "chain " + std::to_string(i)}};
                n["children"] = i + 1 < kDepth ? nlohmann::json::array({first + i + 1}) : roots;
                nodes.push_back(std::move(n));
            }
            j["scenes"][0]["nodes"] = nlohmann::json::array({first});
            return put(std::string("deep_") + impcontract::name(c) + ".gltf", j.dump());
        };
        for (Case c : {Case::UnitTriangle, Case::SkinnedColumn}) {
            const fs::path p = deepen(c);
            bool read = false, valid = false; size_t nodes = 0;
            const bool ran = engine::threads::runWithStack(512 * 1024, [&] {
                const ImportResult d = fe.importScene(p, {});
                read = (bool)d;
                if (d) { nodes = d.scene().nodes.size(); valid = checkScene(d.scene()).empty(); }
            });
            CHECK(ran && read && valid && nodes > (size_t)kDepth,
                  "%s under a %d-deep node chain imports on a 512 KB stack, and the scene is valid (%zu nodes)",
                  impcontract::name(c), kDepth, nodes);
        }
    }

    // ── 5. Bone counts: the file's, whatever the engine's limit ─────────────
    std::printf("5. bone counts\n");
    {
        // A 300-bone rig is read whole: the limit is the cook's to enforce, by
        // name, not the reader's to apply by dropping bones (WO-040).
        const ImportedScene want = impcontract::build::rig(300);
        const ImportResult t = fe.importScene(put("rig300.gltf", gltfw::write(want)), {});
        std::vector<std::string> why;
        if (t) { impcontract::detail::compareGeometry(t.scene(), want, why); impcontract::detail::compareSkeleton(t.scene(), want, why); }
        CHECK(t && why.empty(), "a 300-bone rig is read whole, the top vertices on 'Bone299'%s%s",
              why.empty() ? "" : ": ", why.empty() ? "" : why[0].c_str());

        // More bones than ImportedScene's uint16 joints can index. The reader's
        // bone map was uint16 too, so indices wrapped and weights landed on the
        // wrong bones. It is a skeleton the format cannot carry: Skin/Wrong, and
        // the meshes read static, so the cook refuses with the reason.
        nlohmann::json j = nlohmann::json::parse(gltfw::write(impcontract::expected(Case::SkinnedColumn)));
        auto& nodes = j["nodes"];
        nlohmann::json& joints = j["skins"][0]["joints"];
        j["skins"][0].erase("inverseBindMatrices");         // identity (spec): one per joint would be 4 MB
        std::vector<int> extra;
        const int first = (int)nodes.size(), n = (int)kMaxSceneBones + 10;
        for (int i = 0; i < n; ++i) { nodes.push_back({{"name", "X" + std::to_string(i)}}); joints.push_back(first + i); extra.push_back(first + i); }
        for (int e : extra) j["scenes"][0]["nodes"].push_back(e);
        const ImportResult big = fe.importScene(put("huge_skin.gltf", j.dump()), {});
        bool wrong = false;
        if (big) for (const auto& d : big.scene().dropped)
            wrong |= d.kind == Dropped::Kind::Skin && d.effect == Dropped::Effect::Wrong && d.what.find("bones") != std::string::npos;
        CHECK(big && wrong && !big.scene().skeleton && !big.scene().meshes.empty() && !big.scene().meshes[0].skinned(),
              "a skin of %d joints, more than a uint16 joint can index: Skin/Wrong, read static, no wrapped indices", n);
    }

    fs::remove_all(dir);
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
