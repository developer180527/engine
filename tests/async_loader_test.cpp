// ── async_loader_test — path-key consistency gauntlet (audit C.4) ───────────
// Regression for the half-normalized cache keying: load() keyed its maps by
// normalizeKey(path) but the completion handler stored/looked up the RAW
// path. Consequences on any backslash-bearing path (Windows, mixed callers):
//   - the loaded-results cache never hit → every load() reprocessed the file
//   - waiter callbacks queued under the normalized key were never found by
//     completion → a second caller waited forever
//   - isLoading()/isLoaded()/unload() answered against the wrong key
// POSIX trick: a literal '\' is a valid filename character here, so a file
// named "tri\angle.obj" gives us a raw path that works for fs access while
// normalizeKey() maps it to a different string — exactly the Windows split.
// Runs headless on bgfx Noop (drainOne creates real buffer handles).
//
// Since WO-018 the loader reads COOKED content only, so §2 cooks its fixture
// with the real MeshCooker first (this test links the cook stack for that;
// the loader itself does not). §1's missing file is "not cooked", and with no
// cooker set that is a failure, which is still the waiter path under test.
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "gpu_test_device.h"

#include "runtime/services/async_loader.h"
#include "core/jobs/jobs.h"
#include "assets/asset_storage.h"
#include "assets/cookers/mesh/mesh_cooker.h"
#include "animation/clip_registry.h"
#include "animation/skeleton_registry.h"
#include "gltf_writer.h"
#include "import_contract.h"
#include <assetlib/asset_registry.h>
#include <assetlib/cook_pipeline.h>

namespace fs = std::filesystem;
namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

// Pump the main-thread drain until one asset completes (worker is async).
static bool pumpUntilDrained(AsyncLoader& l, AssetStorage& s, int budgetMs = 15000) {
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - t0).count() < budgetMs) {
        if (l.drainOne(s)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("async_loader_test: path-key consistency gauntlet\n");

    if (!initTestDevice()) return 1;
    jobs::init();

    {
        AssetRegistry    meshes;
        TextureRegistry  textures;
        MaterialRegistry materials;
        AssetStorage storage{meshes, textures, materials};
        AsyncLoader loader;

        // ── 1. Failure path: waiter under a backslash-bearing path ────────
        // (No registry, no cooker: the answer is Failed, "not cooked".)
        // Two callers request the same (missing) asset; the second lands in
        // m_waiters under the normalized key. Pre-fix, completion looked the
        // waiters up under the raw key → cb2 never fired.
        const std::string missing = "no_dir\\no_such_file.obj";
        int cb1 = 0, cb2 = 0;
        loader.load(missing, "m1",
                    [&](const AsyncLoadResult&, const std::string&) { ++cb1; });
        CHECK(loader.isLoading(missing),
              "isLoading() answers true for the raw in-flight path");
        loader.load(missing, "m2",
                    [&](const AsyncLoadResult&, const std::string&) { ++cb2; });

        CHECK(pumpUntilDrained(loader, storage), "failed load drains");
        CHECK(cb1 == 1, "primary callback fired on failure (%d)", cb1);
        CHECK(cb2 == 1, "WAITER callback fired on failure (%d) — the drop bug", cb2);
        CHECK(!loader.isLoading(missing), "in-flight cleared after failure");

        // ── 2. Success path: cache round-trip across separators ──────────
        const fs::path proj = fs::temp_directory_path() / "engine_asyncldr_test";
        fs::remove_all(proj);
        const fs::path dir = proj / "assets";
        fs::create_directories(dir);

        // ── Getting a '\' into the path ─────────────────────────────────────
        // The RAW path must contain a backslash and its normalized twin must
        // not, so normalizeKey's round-trip is exercised. Since WO-018 the
        // loader never opens the SOURCE file (it reads the cooked file the
        // registry names), so the raw path only has to reach the registry:
        // "<root>/assets\tri.obj" is what a Windows caller hands over, and the
        // registry normalizes what it is asked.
        //
        // This used to create a POSIX file literally NAMED "tri\angle.obj" so
        // the source parse could open it. The registry cannot find such a file
        // by any spelling (it stores the '\', and normalizes a query's '\' to
        // '/'), so with a cook in between that fixture could never load.
        const fs::path triFile = dir / "tri.obj";
        {
            std::ofstream f(triFile);
            f << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        }
#if defined(_WIN32)
        const std::string raw = triFile.string();                 // native '\'
#else
        const std::string raw = proj.string() + "/assets\\tri.obj";
#endif

        // Cook it, as the editor's CookService would have (WO-018).
        assetlib::AssetRegistry reg;
        CHECK(reg.open(proj / ".cache" / "registry.db"), "a project registry");
        reg.scan(dir, proj);
        {
            assetlib::CookPipeline pipe(reg, proj, proj / ".cache");
            pipe.registerCooker(std::make_unique<MeshCooker>());
            pipe.cookAll();
        }
        loader.setRegistry(&reg);
        loader.setProjectRoot(proj);
        CHECK(loader.hasCooked(raw), "the fixture cooked (and the registry finds it by the raw path)");

        int okCount = 0;
        loader.load(raw, "tri",
                    [&](const AsyncLoadResult&, const std::string&) { ++okCount; });
        CHECK(pumpUntilDrained(loader, storage), "obj load drains");
        CHECK(okCount == 1, "load callback fired (%d)", okCount);
        CHECK(loader.isLoaded(raw), "isLoaded() true for the raw path");

        std::string fwd = raw;                              // forward-slash twin
        std::replace(fwd.begin(), fwd.end(), '\\', '/');
        CHECK(loader.isLoaded(fwd), "isLoaded() true for the normalized twin");

        // The cache-defeat check: a repeat load() must be served from cache
        // SYNCHRONOUSLY. Pre-fix (stored raw, looked up normalized) this
        // missed and re-dispatched the whole worker parse.
        int cachedCount = 0;
        loader.load(raw, "tri2",
                    [&](const AsyncLoadResult&, const std::string&) { ++cachedCount; });
        CHECK(cachedCount == 1,
              "repeat load served from cache synchronously (%d) — the defeat bug",
              cachedCount);

        // unload() must speak the same key dialect as everything else.
        loader.unload(fwd);                                 // normalized twin
        CHECK(!loader.isLoaded(raw), "unload(normalized) evicts the raw key too");

        reg.close();
        fs::remove_all(proj);
    }   // loader + registries die while bgfx is alive

    // ── 3. A cooked skinned glTF loads WITH its skeleton and clips (BUG-0066) ─
    // The editor spawned every .glb through the runtime glTF importer, which
    // reads static geometry only: a cooked, skinned character had no skeleton,
    // no Animator, and could not animate. The fix routes a glTF to this
    // loader's cooked path when hasCooked() says there is one. This pins both
    // halves the fix relies on: hasCooked() answers truthfully before and after
    // the cook, and the cooked path carries the skeleton and the clip.
    {
        const fs::path root = fs::temp_directory_path() / "engine_asyncldr_skinned";
        fs::remove_all(root);
        fs::create_directories(root / "assets");
        const fs::path src = root / "assets" / "column.gltf";
        { std::ofstream(src) << gltfw::write(impcontract::expected(impcontract::Case::SkinnedColumn)); }
        assetlib::AssetRegistry reg;
        CHECK(reg.open(root / ".cache" / "registry.db"), "a project registry");
        reg.scan(root / "assets", root);

        AsyncLoader loader;
        loader.setRegistry(&reg);
        loader.setProjectRoot(root);
        CHECK(!loader.hasCooked(src.string()), "before the cook, hasCooked() is false");

        assetlib::CookPipeline pipe(reg, root, root / ".cache");
        pipe.registerCooker(std::make_unique<MeshCooker>());
        pipe.cookAll();
        CHECK(loader.hasCooked(src.string()), "after the cook, hasCooked() is true");

        AssetRegistry meshes; TextureRegistry textures; MaterialRegistry materials;
        SkeletonRegistry skeletons; AnimClipRegistry clips;
        AssetStorage storage{meshes, textures, materials, &skeletons, &clips};
        AsyncLoadResult got; bool done = false;
        loader.load(src.string(), "column", [&](const AsyncLoadResult& r, const std::string&) { got = r; done = true; });
        for (int i = 0; i < 5000 && !done; ++i) { loader.drainOne(storage); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        CHECK(done && got.mesh.valid() && got.skeleton.valid() && got.clips.size() == 1,
              "the cooked load returns the mesh, its skeleton and its clip (skeleton %d, %zu clip(s))",
              got.skeleton.valid(), got.clips.size());
        // Close the registry first: Windows cannot delete registry.db while it
        // is open, and the throwing remove_all terminated a test that had passed
        // (0xc0000409, Windows CI, WO-038). Cleanup cannot fail the test.
        reg.close();
        { std::error_code ec; fs::remove_all(root, ec); }
    }

    jobs::shutdown();
    shutdownTestDevice();

    if (g_failures) { std::printf("async_loader_test: %d FAILURE(S)\n", g_failures); return 1; }
    std::printf("async_loader_test: ALL PASS\n");
    return 0;
}
