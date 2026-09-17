// ── colour_grading_test — stage B's plumbing, everything but the pixels ──────
//
// colour_test pins the MATHS (exposure, PBR Neutral, the .cube reader). This
// pins the path from a camera entity to what the output pass receives, which is
// where a correct formula quietly stops mattering: a grading that does not
// serialize, a LUT path that escapes the project, a component the determinism
// gate does not know about, or meta registered at the wrong offsets so the
// inspector edits one field and writes another.
//
// The pixels themselves still have no test — there is no GPU readback in CI.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <flecs.h>
#include <nlohmann/json.hpp>

#include "components/camera.h"
#include "components/colour_grading.h"
#include "components/meta_registry.h"
#include "core/cube_lut.h"
#include "core/display_transform.h"
#include "core/transform.h"
#include "runtime/camera_util.h"
#include "runtime/services/lut_library.h"
#include "runtime/sim_classification.h"
#include "runtime/sim_hash.h"
#include "scene/entity_serializer.h"
#include "scene/reflected_serde.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

namespace fs = std::filesystem;
using nlohmann::json;
using namespace EntitySerde;

static std::string identityCube(int n) {
    std::string s = "LUT_3D_SIZE " + std::to_string(n) + "\n";
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r)
                s += std::to_string(r / double(n - 1)) + " " +
                     std::to_string(g / double(n - 1)) + " " +
                     std::to_string(b / double(n - 1)) + "\n";
    return s;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("colour_grading_test\n");

    // ── 1. Resolving a grading ─────────────────────────────────────────────
    {
        std::printf("\n-- 1. resolved exposure and tone mapper --\n");
        ColourGrading g;
        CHECK(resolvedExposure(g) == 1.0f &&
              resolvedToneMapper(g) == display::ToneMapper::PbrNeutral,
              "a default grading is gain 1 with PBR Neutral — an ungraded scene "
              "keeps its brightness");
        g.exposureEV = 1.0f;
        CHECK(resolvedExposure(g) == 2.0f, "manual +1 stop doubles it");

        ColourGrading p;
        p.exposureMode = (uint8_t)display::ExposureMode::Physical;
        const float expect = display::exposureFromEv100(display::ev100(16.0f, 0.01f, 100.0f));
        CHECK(std::fabs(resolvedExposure(p) - expect) < 1e-9f,
              "physical mode uses aperture/shutter/ISO (%.3g)", (double)resolvedExposure(p));
        p.iso = 0.0f;
        CHECK(resolvedExposure(p) == 1.0f,
              "an invalid physical setting falls back to the manual gain, not NaN");

        ColourGrading bad;
        bad.exposureEV = std::nanf("");
        CHECK(resolvedExposure(bad) == 1.0f, "a NaN exposure resolves to 1");
        bad.exposureEV = 5000.0f;
        CHECK(resolvedExposure(bad) == 1.0f,
              "an exposure that overflows to infinity resolves to 1");
        bad.toneMapper = 7;
        CHECK(resolvedToneMapper(bad) == display::ToneMapper::PbrNeutral,
              "an unknown tone mapper id falls back to the default");
    }

    // ── 2. The scene serializer ────────────────────────────────────────────
    {
        std::printf("\n-- 2. serialization --\n");
        flecs::world w;
        MetaRegistry::registerAll(w);
        SerdeContext ctx;

        ColourGrading g;
        g.exposureMode   = (uint8_t)display::ExposureMode::Physical;
        g.toneMapper     = (uint8_t)display::ToneMapper::None;
        g.exposureEV     = -1.5f;
        g.aperture       = 2.8f;
        g.shutterSeconds = 0.004f;
        g.iso            = 400.0f;
        g.lutPath        = "grades/warm.cube";
        flecs::entity src = w.entity().set<ColourGrading>(g);

        json j;
        saveColourGrading(src, j, ctx);
        flecs::entity dst = w.entity();
        loadColourGrading(dst, j, ctx);
        const ColourGrading* back = dst.try_get<ColourGrading>();
        CHECK(back && back->exposureMode == g.exposureMode &&
              back->toneMapper == g.toneMapper && back->exposureEV == g.exposureEV &&
              back->aperture == g.aperture && back->shutterSeconds == g.shutterSeconds &&
              back->iso == g.iso && back->lutPath == g.lutPath,
              "every field survives save -> load, lutPath included");

        const json hostile = json::parse(R"({
            "exposureMode": 999, "toneMapper": "pbr", "exposureEV": "bright",
            "aperture": [1,2], "iso": null, "lutPath": 42 })");
        flecs::entity h = w.entity();
        loadColourGrading(h, hostile, ctx);
        const ColourGrading* hg = h.try_get<ColourGrading>();
        const ColourGrading def;
        CHECK(hg && hg->exposureMode == def.exposureMode &&
              hg->toneMapper == def.toneMapper && hg->exposureEV == def.exposureEV &&
              hg->aperture == def.aperture && hg->iso == def.iso && hg->lutPath.empty(),
              "wrong-typed and out-of-range fields keep their defaults, without throwing");

        flecs::entity future = w.entity();
        loadColourGrading(future, json::parse(R"({"toneMapper": 9})"), ctx);
        CHECK(future.try_get<ColourGrading>()->toneMapper == 9,
              "an unknown-but-valid tone mapper id is KEPT, so a newer engine's "
              "scene round-trips; resolvedToneMapper refuses it at use");

        json refl = json::object();
        reflected::save(src, refl);
        CHECK(!refl.contains("ColourGrading"),
              "the generic reflected path does NOT also save it — one component, "
              "one place on disk");
    }

    // ── 3. Meta offsets: the inspector edits the field it names ────────────
    // flecs' plain member<T>(name) computes offsets itself, sequentially with
    // alignment — right only while registration order matches the struct.
    // Registered out of order, the inspector would show and edit one field
    // through another's bytes. Pointer-to-member registration cannot drift;
    // this reads each field back through reflection to prove it.
    {
        std::printf("\n-- 3. reflection offsets --\n");
        flecs::world w;
        MetaRegistry::registerAll(w);
        ColourGrading g;
        g.toneMapper = 0; g.exposureEV = 1.25f; g.aperture = 5.6f;
        g.shutterSeconds = 0.125f; g.iso = 800.0f;
        const flecs::entity comp = w.component<ColourGrading>();
        char* s = ecs_ptr_to_json(w, comp, &g);
        const json j = s ? json::parse(s) : json();
        ecs_os_free(s);
        CHECK(j.value("exposureEV", 0.0f) == 1.25f && j.value("aperture", 0.0f) == 5.6f &&
              j.value("shutterSeconds", 0.0f) == 0.125f && j.value("iso", 0.0f) == 800.0f &&
              j.value("toneMapper", 99) == 0,
              "reflection reads every registered member from its real offset (%s)",
              j.dump().c_str());
    }

    // ── 4. The determinism gate knows it is presentation ───────────────────
    {
        std::printf("\n-- 4. classification --\n");
        flecs::world w;
        MetaRegistry::registerAll(w);
        simhash::registerClassification(w);
        w.entity().set<Camera>({}).set<ColourGrading>({});
        std::vector<std::string> un;
        CHECK(simhash::auditCoverage(w, un),
              "a camera with ColourGrading audits clean (%zu unclassified)", un.size());
        const uint64_t before = simhash::hashWorld(w);
        w.each([](flecs::entity, ColourGrading& g) { g.exposureEV = 3.0f; });
        CHECK(simhash::hashWorld(w) == before,
              "and changing the grade does not change the simulation hash");
    }

    // ── 5. The camera finder hands the grading over ────────────────────────
    {
        std::printf("\n-- 5. PrimaryCameraFinder --\n");
        flecs::world w;
        MetaRegistry::registerAll(w);
        flecs::entity cam = w.entity().set<Transform>({}).set<Camera>({});
        PrimaryCameraFinder finder;
        float view[16], proj[16], clear[4];
        const ColourGrading* found = reinterpret_cast<const ColourGrading*>(1);
        CHECK(finder.find(w, view, proj, 1.0f, clear, false, &found) && found == nullptr,
              "a camera with no grading reports null (the out-param is reset)");
        ColourGrading g; g.exposureEV = 2.0f;
        cam.set<ColourGrading>(g);
        CHECK(finder.find(w, view, proj, 1.0f, clear, false, &found) && found &&
              found->exposureEV == 2.0f,
              "and one with a grading reports that camera's");
    }

    // ── 6. LutLibrary ──────────────────────────────────────────────────────
    {
        std::printf("\n-- 6. LutLibrary --\n");
        const fs::path root = fs::temp_directory_path() / "engine_colour_grading_test";
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root / "grades", ec);
        { std::ofstream(root / "grades" / "id.cube") << identityCube(3); }
        { std::ofstream(root / "grades" / "bad.cube") << "LUT_3D_SIZE 3\n0 0 0\n"; }
        { std::ofstream(root.parent_path() / "outside.cube") << identityCube(2); }
        { std::ofstream(root / "grades" / "linear.cube")
              << "LUT_3D_INPUT_RANGE -0.05 4\n" + identityCube(2); }
        { std::ofstream(root / "grades" / "subset.cube")
              << "DOMAIN_MIN 0.1 0.1 0.1\nDOMAIN_MAX 0.9 0.9 0.9\n" + identityCube(2); }

        LutLibrary lib;
        const auto a = lib.get(root, "grades/id.cube");
        CHECK(a && a->size == 3, "a project-relative .cube loads (%u³)", a ? a->size : 0);
        CHECK(lib.get(root, "grades/id.cube").get() == a.get(),
              "and a second request returns the SAME parse — no reread per frame");
        CHECK(!lib.get(root, "../outside.cube"),
              "a path climbing out of the project is refused, though the file exists");
        CHECK(!lib.get(root, (root.parent_path() / "outside.cube").string()),
              "and so is an absolute path");
        CHECK(!lib.get(root, "grades/bad.cube"), "a malformed .cube is refused");
        CHECK(!lib.get(root, "grades/linear.cube"),
              "a LUT built for scene-linear/log input (domain -0.05..4) is refused — "
              "the engine grades display-referred sRGB [0,1]");
        CHECK(lib.get(root, "grades/subset.cube") != nullptr,
              "a domain INSIDE [0,1] is still accepted");
        const size_t n = lib.cachedCount();
        (void)lib.get(root, "grades/bad.cube");
        (void)lib.get(root, "grades/missing.cube");
        (void)lib.get(root, "grades/missing.cube");
        CHECK(lib.cachedCount() == n + 1,
              "failures are cached, so a broken grade is resolved (and logged) once");
        CHECK(!lib.get(root, ""), "an empty path is no grade, not an error");

        ColourGrading g; g.lutPath = "grades/id.cube"; g.exposureEV = -1.0f;
        const DisplayTransform t = resolveDisplayTransform(&g, lib, root);
        CHECK(t.lut.get() == a.get() && t.exposure == 0.5f &&
              t.toneMapper == display::ToneMapper::PbrNeutral,
              "resolveDisplayTransform carries exposure, tone mapper and LUT");
        const DisplayTransform none = resolveDisplayTransform(nullptr, lib, root);
        CHECK(none.exposure == 1.0f && !none.lut &&
              none.toneMapper == display::ToneMapper::PbrNeutral,
              "and a camera with no grading gets the defaults");

        const fs::path other = root / "grades";
        (void)lib.get(other, "id.cube");
        CHECK(lib.cachedCount() == 1,
              "opening a different project drops the previous project's cache");
        fs::remove_all(root, ec);
        fs::remove(root.parent_path() / "outside.cube", ec);
    }

    if (g_failures) {
        std::printf("\ncolour_grading_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\ncolour_grading_test: ALL PASS\n");
    return 0;
}
