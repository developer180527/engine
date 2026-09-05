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
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "gpu_test_device.h"

#include "runtime/services/async_loader.h"
#include "runtime/jobs/jobs.h"
#include "assets/asset_storage.h"

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
        const fs::path dir = fs::temp_directory_path() / "engine_asyncldr_test";
        fs::create_directories(dir);

        // ── Getting a '\' into the path, on both kinds of platform ───────────
        // The point of this section is that the RAW path contains a backslash
        // and the normalized twin does not, so normalizeKey's round-trip is
        // actually exercised. How you obtain that backslash is platform-specific:
        //
        //   POSIX   '\' is an ordinary filename CHARACTER, so a single file
        //           literally named "tri\angle.obj" is the only way to get one
        //           into a path at all.
        //   Windows '\' IS the separator, so a nested directory produces one
        //           natively — and the literal-name trick silently means
        //           something else entirely.
        //
        // The original code used the POSIX trick unconditionally. On Windows
        // `dir / "tri\angle.obj"` is `dir\tri\angle.obj` — a file inside a
        // subdirectory that was never created — so the ofstream failed silently,
        // no file existed, and Assimp reported "Unable to open file". The three
        // failures that followed (isLoaded false, cache miss) were the loader
        // CORRECTLY reporting that a failed parse cached nothing. The fixture was
        // broken, not the code under test, which is the most expensive kind of
        // test bug: it accuses the wrong component.
#if defined(_WIN32)
        fs::create_directories(dir / "tri");
        const fs::path triFile = dir / "tri" / "angle.obj";  // separator IS '\'
#else
        const fs::path triFile = dir / "tri\\angle.obj";     // literal '\' in name
#endif
        {
            std::ofstream f(triFile);
            f << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
        }
        const std::string raw = triFile.string();          // contains '\'

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

        fs::remove_all(dir);
    }   // loader + registries die while bgfx is alive

    jobs::shutdown();
    shutdownTestDevice();

    if (g_failures) { std::printf("async_loader_test: %d FAILURE(S)\n", g_failures); return 1; }
    std::printf("async_loader_test: ALL PASS\n");
    return 0;
}
