// ── residency_test — bounded asset residency (audit Q6) ─────────────────────
// The loaded-mesh cache used to grow without bound: nothing ever evicted.
// This gauntlet proves the residency contract: a byte budget, LRU eviction
// order, in-use protection (a mesh a live MeshRenderer references never
// vanishes under the renderer), and transparent reload of evicted paths.
// Uses REAL cooked meshes (MeshCooker on a tiny OBJ) streamed through the
// REAL async path. Headless on bgfx Noop. Exits non-zero on first failure.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_set>

#include "gpu_test_device.h"

#include "runtime/services/asset_service.h"
#include "assets/cookers/mesh/mesh_cooker.h"
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"

namespace fs = std::filesystem;
namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

static void settle(AssetService& svc, int budgetMs = 10000) {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count() < budgetMs) {
        while (svc.drainUploads()) {}
        if (svc.pendingCount() == 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("residency_test: bounded asset residency gauntlet\n");

    if (!initTestDevice()) return 1;

    // Project layout in temp: cook one tiny OBJ, then clone the cooked file
    // so we have four distinct streamable meshes of identical size.
    const fs::path root  = fs::temp_directory_path() / "engine_residency_test";
    const fs::path cache = root / ".cache" / "meshs";
    fs::remove_all(root);
    fs::create_directories(cache);
    {
        const fs::path obj = root / "tri.obj";
        { std::ofstream f(obj); f << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"; }
        MeshCooker cooker;
        assetlib::CookContext ctx;
        ctx.sourcePath = obj;
        ctx.outputPath = cache / "a.cooked";
        if (!cooker.cook(ctx).success) { std::printf("FAIL cook\n"); return 1; }
        for (const char* n : {"b.cooked", "c.cooked", "d.cooked"})
            fs::copy_file(cache / "a.cooked", cache / n);
    }

    {
        AssetRegistry    meshes;
        TextureRegistry  textures;
        MaterialRegistry materials;
        AssetService svc({meshes, textures, materials});
        svc.setProjectRoot(root);

        auto loadAll = [&](std::initializer_list<const char*> names) {
            for (const char* n : names)
                svc.loadMeshAsync((std::string("meshs/") + n).c_str());
            settle(svc);
        };

        // ── 1. Load a..d; measure one mesh's cost; set budget to ~2 ──────
        loadAll({"a.cooked", "b.cooked", "c.cooked", "d.cooked"});
        const uint64_t all4 = svc.residentBytes();
        CHECK(all4 > 0, "4 meshes resident (%llu bytes)",
              (unsigned long long)all4);
        const uint64_t one = all4 / 4;

        // Touch order (oldest → newest): a, b, c, d.
        for (const char* n : {"a.cooked", "b.cooked", "c.cooked", "d.cooked"})
            (void)svc.queryMesh((std::string("meshs/") + n).c_str());

        // ── 2. LRU eviction to a 2-mesh budget ────────────────────────────
        svc.setResidencyBudget(one * 2);
        const size_t evicted = svc.evictOverBudget({});
        CHECK(evicted == 2, "evicted exactly 2 LRU meshes (%zu)", evicted);
        CHECK(svc.queryMesh("meshs/a.cooked") == 0, "a (oldest) evicted");
        CHECK(svc.queryMesh("meshs/b.cooked") == 0, "b (2nd oldest) evicted");
        CHECK(svc.queryMesh("meshs/c.cooked") != 0, "c survives");
        CHECK(svc.queryMesh("meshs/d.cooked") != 0, "d survives");
        CHECK(svc.residentBytes() <= one * 2, "resident bytes within budget");

        // ── 3. In-use protection: the oldest mesh is referenced ──────────
        loadAll({"a.cooked", "b.cooked"});          // back to 4 resident
        // Re-stamp so c is oldest now: touch order d, a, b (c untouched).
        for (const char* n : {"d.cooked", "a.cooked", "b.cooked"})
            (void)svc.queryMesh((std::string("meshs/") + n).c_str());
        const uint32_t cId = [&] {
            // c is coldest — but pretend a live MeshRenderer holds it.
            // (query would re-stamp it; peek via a throwaway query on the
            // OTHERS then read c last... simpler: it was loaded above.)
            return svc.queryMesh("meshs/c.cooked");
        }();
        // c is now NEWEST after that query; make it coldest again by
        // touching everything else after it.
        for (const char* n : {"d.cooked", "a.cooked", "b.cooked"})
            (void)svc.queryMesh((std::string("meshs/") + n).c_str());

        const size_t evicted2 = svc.evictOverBudget({cId});   // c is in use
        CHECK(evicted2 == 2, "evicted 2 with in-use skip (%zu)", evicted2);
        CHECK(svc.queryMesh("meshs/c.cooked") == cId,
              "in-use mesh survived despite being coldest — the guard");

        // ── 4. Transparent reload of an evicted path ──────────────────────
        const char* reloadMe = svc.queryMesh("meshs/d.cooked") == 0
                             ? "meshs/d.cooked" : "meshs/a.cooked";
        svc.setResidencyBudget(0);                  // lift budget for reload
        svc.loadMeshAsync(reloadMe);
        settle(svc);
        CHECK(svc.queryMesh(reloadMe) != 0,
              "evicted path reloads transparently (%s)", reloadMe);

        // ── 4. The guard that decides whether a sweep is worth its scan ─────
        // frameBegin builds an `inUse` set by walking EVERY entity in EVERY
        // world before calling evictOverBudget — 0.463 ms at 50 000 entities,
        // measured. evictOverBudget then discards it on its first line whenever
        // there is no budget or the cache is under it, and the default budget is
        // 0 (unbounded), so in the editor and any build without meshBudgetMB the
        // scan was pure waste, once a second, forever (BUG-0047).
        //
        // residencySweepNeeded() is that precondition, asked BEFORE paying for
        // the scan. It has to agree with evictOverBudget exactly in both
        // directions: a false yes brings the waste back, and a false NO lets the
        // cache grow unbounded — which is the bug residency exists to fix, so
        // the wrong guard is worse than no guard.
        std::printf("\n-- 4. sweep guard --\n");

        svc.setResidencyBudget(0);
        CHECK(!svc.residencySweepNeeded(),
              "no budget -> no sweep (the editor default; this is the 0.46 ms)");

        const uint64_t resident = svc.residentBytes();
        CHECK(resident > 0, "something is resident to reason about (%llu bytes)",
              (unsigned long long)resident);

        svc.setResidencyBudget(resident * 4);
        CHECK(!svc.residencySweepNeeded(), "under budget -> no sweep");

        svc.setResidencyBudget(resident / 2);
        CHECK(svc.residencySweepNeeded(),
              "OVER budget -> sweep, or residency does nothing at all");

        // The guard must agree with the thing it guards: when it says yes,
        // evictOverBudget must actually evict something.
        const size_t swept = svc.evictOverBudget({});
        CHECK(swept > 0,
              "guard said sweep and evictOverBudget agreed (%zu evicted)", swept);
        svc.setResidencyBudget(0);
    }

    shutdownTestDevice();
    fs::remove_all(root);

    if (g_failures) { std::printf("residency_test: %d FAILURE(S)\n", g_failures); return 1; }
    std::printf("residency_test: ALL PASS\n");
    return 0;
}
