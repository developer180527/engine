// ── stress_assets — garbage-in fuzz for the asset + nav pipeline ────────────
// Feeds degenerate / NaN / absurdly-huge geometry to NavService and malformed
// files to the importers, asserting GRACEFUL failure (no crash, no hang, no
// OOM) rather than a happy-path assumption. Found: NavService would hand NaN/
// huge bounds straight to Recast and rasterize a giant heightfield → guarded
// with a finite-bounds + grid-size cap (nav_service.cpp).
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <bgfx/bgfx.h>

#include "runtime/services/nav_service.h"
#include "assets/importers/importer_registry.h"
#include "assets/importers/gltf_importer.h"
#include "assets/importers/assimp_importer.h"
#include "assets/asset_storage.h"
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

int main(int argc, char** argv) {
    std::printf("stress_assets: garbage-in fuzz (nav + importers)\n");
    bgfx::renderFrame();
    bgfx::Init init; init.type = bgfx::RendererType::Noop;
    init.resolution.width = 16; init.resolution.height = 16;
    if (!bgfx::init(init)) { std::printf("stress_assets: FAIL — bgfx Noop\n"); return 1; }

    // ── NavService fuzz: all must return false, none may crash/hang ──────────
    {
        nav::NavService nav;
        const float nanf = std::numeric_limits<float>::quiet_NaN();

        CHECK(!nav.build(nullptr, 0, nullptr, 0), "empty geometry rejected");

        const float degenerate[9] = {0,0,0, 0,0,0, 0,0,0};   // zero-area tri
        const int   tri[3] = {0,1,2};
        CHECK(!nav.build(degenerate, 3, tri, 1) || !nav.ready(),
              "zero-area triangle → no navmesh (not a crash)");

        const float nanVerts[9] = {0,0,0, 1,nanf,0, 0,0,1};
        CHECK(!nav.build(nanVerts, 3, tri, 1), "NaN vertex rejected (no Recast hang)");

        const float huge[9] = {-1e18f,0,-1e18f, 1e18f,0,-1e18f, 0,0,1e18f};
        CHECK(!nav.build(huge, 3, tri, 1), "astronomical extents rejected (no giant grid)");

        // Out-of-range indices must not read out of bounds and crash Recast.
        const float ok[12] = {-5,0,-5, 5,0,-5, 5,0,5, -5,0,5};
        const int   badIdx[6] = {0,2,1, 99,98,97};   // second tri indexes nowhere
        nav.build(ok, 4, badIdx, 2);                 // may fail; must not crash
        CHECK(true, "out-of-range indices handled without a crash");
    }

    // ── Importer fuzz: malformed/missing files must fail gracefully ─────────
    {
        AssetRegistry meshes; TextureRegistry tex; MaterialRegistry mat;
        AssetStorage storage{meshes, tex, mat};
        ImporterRegistry imp;
        imp.registerImporter(std::make_unique<GltfImporter>());
        imp.registerImporter(std::make_unique<AssimpImporter>());

        MeshImportResult r = imp.load("/does/not/exist.gltf", storage);
        CHECK(!r.success, "missing file → graceful fail, no crash");

        // Any malformed sample paths passed on the CLI (e.g. cgltf fuzz data).
        int fuzzed = 0;
        for (int a = 1; a < argc; ++a) {
            MeshImportResult f = imp.load(argv[a], storage);
            std::printf("        fuzz %s -> %s\n", argv[a],
                        f.success ? "loaded" : f.error.c_str());
            ++fuzzed;   // the only requirement: we returned, didn't crash
        }
        CHECK(true, "%d malformed/sample file(s) survived without a crash", fuzzed);
    }

    bgfx::shutdown();
    if (g_failures) { std::printf("stress_assets: FAIL — %d\n", g_failures); return 1; }
    std::printf("stress_assets: PASS — garbage-in handled gracefully\n");
    return 0;
}
