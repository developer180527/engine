// ── import_frontend_contract_test — the import-frontend contract, pinned (WO-010)
//
// Four things, each of which must be true before any real front end is judged
// by this suite:
//   1. the registry's "no front end" answer is the contract's stub (Unsupported);
//   2. the reference scenes themselves satisfy imp::checkScene;
//   3. checkScene catches each violation it claims to — every check is shown
//      failing on a scene broken in exactly that way;
//   4. the suite passes the fake, and FAILS front ends broken in each way a real
//      parser plausibly breaks: flipped winding, unconverted units, ignored node
//      transforms, unnormalised weights, an unreported loss, an empty scene
//      instead of Empty, a missing file that "succeeds". A suite that cannot
//      fail proves nothing about the front ends it passes.
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "import_contract.h"

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__); std::printf("\n"); ++g_failures; } \
                              else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } } while (0)

using namespace imp;
using impcontract::Case;

static bool hasViolation(const ImportedScene& s, const char* check) {
    for (const Violation& v : checkScene(s))
        if (std::string(v.check) == check) return true;
    return false;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("import_frontend_contract_test\n");

    // ── 1. The registry: routing, and the stub answer ───────────────────────
    std::printf("1. registry\n");
    {
        ImportFrontendRegistry reg;
        auto fake = std::make_unique<FakeFrontend>();
        fake->set("a.fake", impcontract::expected(Case::UnitTriangle));
        reg.add(std::move(fake));

        const ImportResult none = reg.importScene("model.usdz");
        CHECK(!none && none.error().kind == ImportError::Kind::Unsupported
                    && none.error().message.find(".usdz") != std::string::npos,
              "an extension nobody handles is Unsupported, naming it: %s",
              none ? "(a scene!)" : none.error().message.c_str());
        CHECK(reg.forPath("A.FAKE") != nullptr, "extensions match case-insensitively");
        const ImportResult ok = reg.importScene("a.fake");
        CHECK(ok && ok.scene().source == "a.fake", "a claimed extension routes to its front end");
    }

    // ── 2. The references are valid scenes ─────────────────────────────────
    std::printf("2. reference scenes pass checkScene\n");
    for (Case c : impcontract::kAllCases) {
        if (c == Case::EmptyFile) continue;
        const auto v = checkScene(impcontract::expected(c));
        CHECK(v.empty(), "%s: %s", impcontract::name(c), v.empty() ? "valid" : (std::string(v[0].check) + ": " + v[0].detail).c_str());
    }

    // ── 3. Every check fails on the scene broken its way ────────────────────
    std::printf("3. checkScene catches each violation\n");
    {
        const ImportedScene tri  = impcontract::expected(Case::UnitTriangle);
        const ImportedScene skin = impcontract::expected(Case::SkinnedColumn);
        const float nan = std::nanf("");

        struct Breakage { const char* what; const char* check; ImportedScene base; std::function<void(ImportedScene&)> fn; };
        const Breakage cases[] = {
            {"no triangles, no clips",            "empty",     tri,  [](ImportedScene& s) { s.meshes.clear(); s.nodes = {s.nodes[0]}; s.nodes[0].meshes.clear(); }},
            {"a normal missing",                  "streams",   tri,  [](ImportedScene& s) { s.meshes[0].normals.pop_back(); }},
            {"a partial UV stream",               "streams",   tri,  [](ImportedScene& s) { s.meshes[0].uv0.pop_back(); }},
            {"joints without weights",            "streams",   skin, [](ImportedScene& s) { s.meshes[0].weights.clear(); }},
            {"a NaN position",                    "finite",    tri,  [nan](ImportedScene& s) { s.meshes[0].positions[1].y = nan; }},
            {"a tangent with w = 0",              "tangents",  tri,  [](ImportedScene& s) { s.meshes[0].tangents.assign(3, {1, 0, 0, 0}); }},
            {"an index past the vertices",        "indices",   tri,  [](ImportedScene& s) { s.meshes[0].indices[2] = 7; }},
            {"indices not a multiple of 3",       "indices",   tri,  [](ImportedScene& s) { s.meshes[0].indices.push_back(0); s.meshes[0].submeshes[0].indexCount = 4; }},
            {"a gap between submeshes",           "submeshes", impcontract::expected(Case::TwoMaterialQuad), [](ImportedScene& s) { s.meshes[0].submeshes[1].firstIndex = 4; }},
            {"submeshes that stop short",         "submeshes", impcontract::expected(Case::TwoMaterialQuad), [](ImportedScene& s) { s.meshes[0].submeshes.pop_back(); }},
            {"a submesh naming no material",      "submeshes", tri,  [](ImportedScene& s) { s.meshes[0].submeshes[0].material = 5; }},
            {"weights summing to 0.9",            "weights",   skin, [](ImportedScene& s) { s.meshes[0].weights[2] = {0.45f, 0.45f, 0, 0}; }},
            {"a negative weight",                 "weights",   skin, [](ImportedScene& s) { s.meshes[0].weights[0] = {1.2f, -0.2f, 0, 0}; }},
            {"a weighted joint past the bones",   "weights",   skin, [](ImportedScene& s) { s.meshes[0].joints[4] = {0, 9, 0, 0}; }},
            {"skinned, no skeleton",              "weights",   skin, [](ImportedScene& s) { s.skeleton.reset(); s.clips.clear(); }},
            {"a node whose parent comes later",   "nodes",     impcontract::expected(Case::NodeTransforms), [](ImportedScene& s) { s.nodes[1].parent = 2; }},
            {"a mesh on no node",                 "nodes",     tri,  [](ImportedScene& s) { s.meshes.push_back(s.meshes[0]); }},
            {"a node naming no mesh",             "nodes",     tri,  [](ImportedScene& s) { s.nodes[0].meshes.push_back(3); }},
            {"a texture that is path AND bytes",  "textures",  tri,  [](ImportedScene& s) { s.materials[0].baseColor = {"a.png", {1, 2}, "a"}; }},
            {"embedded bytes with no name",       "textures",  tri,  [](ImportedScene& s) { s.materials[0].normal.embedded = {1, 2}; }},
            {"a bone before its parent",          "skeleton",  skin, [](ImportedScene& s) { s.skeleton->bones[0].parent = 1; }},
            {"two bones with one name",           "skeleton",  skin, [](ImportedScene& s) { s.skeleton->bones[1].name = "Hips"; }},
            {"an inverseBind that does not invert","skeleton", skin, [](ImportedScene& s) { s.skeleton->bones[1].inverseBind = {}; }},
            {"a clip animating a non-bone",       "clips",     skin, [](ImportedScene& s) { s.clips[0].tracks[0].bone = "Tail"; }},
            {"keys out of order",                 "clips",     skin, [](ImportedScene& s) { std::swap(s.clips[0].tracks[0].rotation[0], s.clips[0].tracks[0].rotation[1]); }},
            {"a key past the duration",           "clips",     skin, [](ImportedScene& s) { s.clips[0].tracks[0].rotation[1].time = 2.0f; }},
            {"a clip with no skeleton",           "clips",     tri,  [](ImportedScene& s) { Clip c; c.name = "Swing"; c.duration = 1; s.clips = {c}; }},
            {"a dropped entry that says nothing", "dropped",   tri,  [](ImportedScene& s) { s.dropped = {{Dropped::Kind::Camera, Dropped::Effect::Less, 1, ""}}; }},
        };
        for (const Breakage& b : cases) {
            ImportedScene s = b.base;
            b.fn(s);
            CHECK(hasViolation(s, b.check), "%-36s -> %s", b.what, b.check);
        }
    }

    // ── 4. The suite passes the fake … ──────────────────────────────────────
    std::printf("4. the contract suite\n");
    {
        FakeFrontend fake;
        const impcontract::Report r = impcontract::run(impcontract::fakeSubject(fake));
        for (const auto& f : r.failures) std::printf("        %s\n", f.c_str());
        CHECK(r.failures.empty() && r.skipped.empty() && r.passed == (int)std::size(impcontract::kAllCases) + 1,
              "the fake passes every case plus MissingFile (%d passed, %zu failed, %zu skipped)",
              r.passed, r.failures.size(), r.skipped.size());
    }

    // ── … and fails front ends broken the ways real parsers break ──────────
    // Each is the fake answering one case WRONGLY; the suite must name that case.
    std::printf("5. the suite fails broken front ends\n");
    {
        auto brokenOn = [](Case c, std::function<void(ImportedScene&)> spoil) {
            FakeFrontend fake;
            impcontract::Subject subj = impcontract::fakeSubject(fake);
            ImportedScene s = impcontract::expected(c);
            spoil(s);
            fake.set(std::string(impcontract::name(c)) + ".fake", s);
            return impcontract::run(subj);
        };
        struct Wrong { const char* what; Case c; std::function<void(ImportedScene&)> spoil; };
        const Wrong wrongs[] = {
            {"flipped winding",                 Case::UnitTriangle,          [](ImportedScene& s) { std::swap(s.meshes[0].indices[1], s.meshes[0].indices[2]); }},
            {"UVs with a bottom-left origin",   Case::UnitTriangle,          [](ImportedScene& s) { for (auto& uv : s.meshes[0].uv0) uv.y = 1 - uv.y; }},
            {"node transforms ignored",         Case::NodeTransforms,        [](ImportedScene& s) { for (auto& n : s.nodes) n.local = {}; }},
            {"instancing collapsed to one",     Case::NodeTransforms,        [](ImportedScene& s) { s.nodes[2].meshes.clear(); }},
            {"submeshes merged, one material",  Case::TwoMaterialQuad,       [](ImportedScene& s) { s.meshes[0].submeshes = {{0, 6, 0}}; }},
            {"centimetres not converted",       Case::AuthoredCentimetreZUp, [](ImportedScene& s) { for (auto& p : s.meshes[0].positions) { p.x *= 100; p.y *= 100; p.z *= 100; } }},
            {"Z-up not converted",              Case::AuthoredCentimetreZUp, [](ImportedScene& s) { for (auto& p : s.meshes[0].positions) { std::swap(p.y, p.z); p.y = -p.y; } }},
            {"weights on the wrong bone",       Case::SkinnedColumn,         [](ImportedScene& s) { s.meshes[0].joints[0] = {1, 0, 0, 0}; }},
            {"a bone re-parented",              Case::SkinnedColumn,         [](ImportedScene& s) { s.skeleton->bones[1].parent = -1; }},
            {"the clip's last key wrong",       Case::SkinnedColumn,         [](ImportedScene& s) { s.clips[0].tracks[0].rotation[1].value = {0, 0, 0, 1}; }},
            {"a loss not reported",             Case::Unrepresentable,       [](ImportedScene& s) { s.dropped.pop_back(); }},
            {"a loss with the wrong effect",    Case::Unrepresentable,       [](ImportedScene& s) { s.dropped[0].effect = Dropped::Effect::Wrong; }},
        };
        for (const Wrong& w : wrongs) {
            const impcontract::Report r = brokenOn(w.c, w.spoil);
            CHECK(r.failedCase(w.c), "%-32s -> %s fails%s", w.what, impcontract::name(w.c),
                  r.failures.empty() ? "" : (": " + r.failures[0]).c_str());
        }

        FakeFrontend emptyScene;
        impcontract::Subject subj = impcontract::fakeSubject(emptyScene);
        emptyScene.set("EmptyFile.fake", ImportedScene{});
        CHECK(impcontract::run(subj).failedCase(Case::EmptyFile),
              "an empty ImportedScene instead of ImportError::Empty -> EmptyFile fails");

        FakeFrontend succeedsOnMissing;
        impcontract::Subject subj2 = impcontract::fakeSubject(succeedsOnMissing);
        succeedsOnMissing.set("/nonexistent/wo010-contract/missing.fake", impcontract::expected(Case::UnitTriangle));
        bool caught = false;
        for (const auto& f : impcontract::run(subj2).failures) caught |= f.rfind("MissingFile:", 0) == 0;
        CHECK(caught, "a scene for a file that does not exist -> MissingFile fails");
    }

    // ── 6. A case a format cannot express is SKIPPED, never passed ─────────
    std::printf("6. skipped cases are reported\n");
    {
        FakeFrontend fake;
        impcontract::Subject subj = impcontract::fakeSubject(fake);
        auto inner = subj.source;
        subj.source = [inner](Case c) -> std::optional<std::filesystem::path> {
            if (c == Case::SkinnedColumn || c == Case::AuthoredCentimetreZUp) return std::nullopt;   // "like OBJ"
            return inner(c);
        };
        const impcontract::Report r = impcontract::run(subj);
        CHECK(r.skipped.size() == 2 && r.passed == (int)std::size(impcontract::kAllCases) - 2 + 1,
              "two inexpressible cases are skipped (%zu) and not counted as passed (%d)", r.skipped.size(), r.passed);
    }

    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
