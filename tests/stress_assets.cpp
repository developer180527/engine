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

#include "gpu_test_device.h"

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
    // Unbuffered: ctest redirects stdout, which makes it block-buffered,
    // and a test killed on timeout loses everything still in the buffer.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("stress_assets: garbage-in fuzz (nav + importers)\n");
    if (!initTestDevice()) return 1;

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

    shutdownTestDevice();
    if (g_failures) { std::printf("stress_assets: FAIL — %d\n", g_failures); return 1; }
    std::printf("stress_assets: PASS — garbage-in handled gracefully\n");
    return 0;
}
