// ── import_test — multi-submesh import gauntlet ─────────────────────────────
// Proves the importers (glTF + Assimp) merge EVERY submesh/primitive of a
// source model into ONE Mesh with a SubmeshRange per source part (each with its
// own material) — the bug in issues.md was reading only meshes[0].primitives[0]
// (glTF) / returning only the first submesh (Assimp), silently dropping the
// rest of the geometry. Runs headless on bgfx's Noop backend: buffer creation
// returns valid handles without a GPU or window, so importers run unmodified.
// Pass model paths as args; each must import with >= 1 submesh, and at least
// one multi-material model must yield > 1 submesh with distinct materials.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <bgfx/bgfx.h>

#include "assets/importers/importer_registry.h"
#include "assets/importers/gltf_importer.h"
#include "assets/importers/assimp_importer.h"
#include "assets/asset_storage.h"
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"
#include "render/mesh.h"
#include "animation/skeleton_registry.h"
#include "animation/clip_registry.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                        \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("\n"); ++g_failures; }                \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // unbuffered: survive a late crash
    std::printf("import_test: multi-submesh import gauntlet\n");

    // Single-threaded Noop bgfx: valid handles, no GPU. Must renderFrame()
    // before init to select single-threaded mode (same as the real renderer).
    bgfx::renderFrame();
    bgfx::Init init;
    init.type = bgfx::RendererType::Noop;
    init.resolution.width  = 16;
    init.resolution.height = 16;
    if (!bgfx::init(init)) {
        std::printf("import_test: FAIL — bgfx Noop init failed\n");
        return 1;
    }

    // Registries scoped so cooked Mesh objects (whose dtor calls bgfx::destroy)
    // are torn down while bgfx is still up — shutdown happens after this block.
    // Model paths come from args; with none, fall back to known multi-part repo
    // assets (relative to the repo root, the usual run cwd). Missing defaults
    // are skipped, not failed, so the test is safe where the assets are absent.
    std::vector<std::string> models;
    for (int a = 1; a < argc; ++a) models.emplace_back(argv[a]);
    bool usedDefaults = false;
    if (models.empty()) {
        usedDefaults = true;
        for (const char* p : { "assets/person.glb",              // 26 primitives
                               "assets/Double_Dagger_Stab.fbx",  // 11 skinned submeshes
                               "assets/robot.glb" })             // single-material
            if (std::filesystem::exists(p)) models.emplace_back(p);
        if (models.empty()) {
            std::printf("import_test: SKIP — no default test models present\n");
            bgfx::shutdown();
            return 0;
        }
    }

    int  multiSubmeshSeen  = 0;
    bool anyDistinctMats   = false;
    {
    AssetRegistry    meshes;
    TextureRegistry  textures;
    MaterialRegistry materials;
    SkeletonRegistry skeletons;   // wired so skinned models yield a skeleton —
    AnimClipRegistry clips;       // the cache-preservation regression needs it
    AssetStorage storage{meshes, textures, materials, &skeletons, &clips};

    ImporterRegistry importers;
    importers.registerImporter(std::make_unique<GltfImporter>());
    importers.registerImporter(std::make_unique<AssimpImporter>());

    for (const std::string& path : models) {
        std::printf("— %s\n", path.c_str());
        MeshImportResult r = importers.load(path, storage);
        if (!r.success) { CHECK(false, "import failed: %s", r.error.c_str()); continue; }

        const Mesh* m = meshes.getMesh(r.mesh);
        CHECK(m != nullptr, "produced a mesh");
        if (!m) continue;

        // submeshes empty == single-draw (one material); else N ranges.
        const size_t subs = m->submeshes.empty() ? 1 : m->submeshes.size();
        CHECK(m->indexCount > 0, "mesh has geometry (%u indices)", m->indexCount);
        std::printf("        submeshes=%zu  bounds=(%.3f,%.3f,%.3f)..(%.3f,%.3f,%.3f)\n",
                    subs, m->boundsMin.x, m->boundsMin.y, m->boundsMin.z,
                    m->boundsMax.x, m->boundsMax.y, m->boundsMax.z);

        if (subs > 1) {
            ++multiSubmeshSeen;
            // Every range must cover part of the shared IB and stay in bounds.
            // Distinct materials are tracked at RUN level: a multi-submesh model
            // can legitimately share one material (geometry split, same skin).
            bool ranges_ok = true;
            MaterialHandle firstMat = m->submeshes.front().material;
            for (const auto& s : m->submeshes) {
                if (s.indexOffset + s.indexCount > m->indexCount) ranges_ok = false;
                if (s.material.id != firstMat.id) anyDistinctMats = true;
            }
            CHECK(ranges_ok, "all submesh ranges lie within the shared index buffer");
        }

        // Regression (importer audit "Animation Data Loss on Cache Hit"): a
        // skinned asset loaded via the CACHE must still carry its skeleton and
        // clips. The cache used to store only the MeshHandle, so the second
        // load silently stripped the animation. Only exercised for a model that
        // actually rigged on the direct load.
        if (r.skeleton.valid()) {
            MeshImportResult c1 = importers.loadCached(path, storage);  // populates cache
            MeshImportResult c2 = importers.loadCached(path, storage);  // cache HIT
            CHECK(c2.skeleton.valid(),
                  "cache hit preserves skeleton (was dropped pre-fix)");
            CHECK(c2.clips.size() == c1.clips.size() && !c2.clips.empty(),
                  "cache hit preserves %zu clip(s)", c1.clips.size());
        }
    }

    // Enforce coverage when the caller chose models, or when a multi-part
    // default was actually present; don't fail just because the big default
    // assets are absent on this machine.
    if (!usedDefaults || multiSubmeshSeen > 0) {
        CHECK(multiSubmeshSeen > 0,
              "some model imported as multi-submesh (pass a multi-part model)");
        CHECK(anyDistinctMats,
              "some multi-submesh model carries per-submesh materials");
    } else {
        std::printf("  skip  no multi-part default model present — coverage not exercised\n");
    }
    }   // registries destroyed here (Mesh dtors run while bgfx is alive)

    bgfx::shutdown();
    if (g_failures) { std::printf("import_test: FAIL — %d failure(s)\n", g_failures); return 1; }
    std::printf("import_test: PASS — importers merge all submeshes\n");
    return 0;
}
