// ── cook_infra_test — cook infrastructure gauntlet ───────────────────────────
// Regressions for the cook-layer audit (modules/assetlib/src/IMPROVEMENTS.md).
// Covers the scheduler, the DDC, the manifest parser, and the registry scanner
// — the subtle paths that end-to-end cooks exercise only by luck:
//   1. TaskGraph honours dependency order
//   2. TaskGraph dispatches longest-first (cost-weighted, LPT)
//   3. TaskGraph reports the ACTUAL cycle (A -> B -> C -> A), never hangs
//   4. TaskGraph cancellation stops dispatch and still drains
//   5. DdcStore::storeBytes survives concurrent same-key writers (the temp
//      name must be unique per THREAD, not just per process)
//   6. ddcFetchRecord rejects hostile manifest member names (path traversal,
//      Windows drive-relative "C:evil") — a shared DDC is remote input
//   7. AssetRegistry::scan still re-points a MOVED file onto its old UUID
//      (the O(N) hash index must not change move-detection behaviour)
//   8. scan never claims one record for two different moved files
// Headless, no GPU. Exits non-zero on first failure.
#include <assetlib/asset_registry.h>
#include <assetlib/ddc.h>
#include <assetlib/ddc_manifest.h>
#include <assetlib/task_graph.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "test_watchdog.h"

namespace fs = std::filesystem;
namespace { int g_failures = 0; }

#define CHECK(cond, ...) do {                                        \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);              \
                   std::printf("\n"); ++g_failures; }                \
    else         { std::printf("  ok    " __VA_ARGS__);              \
                   std::printf("\n"); }                             \
} while (0)

static void writeFile(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

// ── 1/2. Ordering ────────────────────────────────────────────────────────────
static void testGraphOrdering() {
    std::printf("[graph] dependency + cost ordering\n");
    {
        assetlib::TaskGraph g;
        std::mutex mtx;
        std::vector<std::string> order;
        auto record = [&](const char* n) {
            std::lock_guard<std::mutex> lk(mtx); order.push_back(n);
        };
        // c depends on b depends on a.
        const int a = g.add("a", 1, [&] { record("a"); });
        const int b = g.add("b", 1, [&] { record("b"); });
        const int c = g.add("c", 1, [&] { record("c"); });
        g.addEdge(a, b);
        g.addEdge(b, c);
        assetlib::TaskGraph::Options o; o.workers = 4;
        const size_t ran = g.run(o);
        CHECK(ran == 3, "all 3 tasks ran (%zu)", ran);
        CHECK(order.size() == 3 && order[0] == "a" && order[1] == "b"
              && order[2] == "c", "edges honoured (a,b,c)");
    }
    {
        // Single worker + distinct costs: dispatch must be biggest-first.
        assetlib::TaskGraph g;
        std::vector<std::string> order;
        g.add("small",  10,        [&] { order.push_back("small");  });
        g.add("huge",   1000000,   [&] { order.push_back("huge");   });
        g.add("medium", 5000,      [&] { order.push_back("medium"); });
        assetlib::TaskGraph::Options o;
        o.workers = 1;
        o.memBudget = (size_t)1 << 30;
        g.run(o);
        CHECK(order.size() == 3 && order[0] == "huge" && order[1] == "medium"
              && order[2] == "small", "longest-processing-first dispatch");
    }
}

// ── 3. Cycle ─────────────────────────────────────────────────────────────────
static void testGraphCycle() {
    std::printf("[graph] cycle detection terminates and names the loop\n");
    assetlib::TaskGraph g;
    std::atomic<int> ran{0};
    const int a = g.add("A", 1, [&] { ++ran; });
    const int b = g.add("B", 1, [&] { ++ran; });
    const int c = g.add("C", 1, [&] { ++ran; });
    const int d = g.add("D", 1, [&] { ++ran; });
    g.addEdge(a, b); g.addEdge(b, c); g.addEdge(c, a);   // A->B->C->A
    (void)d;                                             // d is reachable

    const auto t0 = std::chrono::steady_clock::now();
    assetlib::TaskGraph::Options o; o.workers = 2;
    const size_t drained = g.run(o);
    const double sec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - t0).count();

    CHECK(sec < 10.0, "returned instead of deadlocking (%.2fs)", sec);
    CHECK(drained == 1, "only the acyclic task drained (%zu)", drained);
    CHECK(ran.load() == 1, "cyclic tasks never ran (%d)", ran.load());
    std::printf("        (a 'cycle: ...' line above should name A/B/C)\n");
}

// ── 4. Cancellation ──────────────────────────────────────────────────────────
static void testGraphCancel() {
    std::printf("[graph] cancellation stops dispatch, drains in flight\n");
    assetlib::TaskGraph g;
    std::atomic<int> started{0}, doneCount{0};
    for (int i = 0; i < 64; ++i)
        g.add("t" + std::to_string(i), 1,
              [&] { ++started; std::this_thread::sleep_for(
                        std::chrono::milliseconds(5)); },
              [&] { ++doneCount; });

    std::atomic<bool> keepGoing{true};
    assetlib::TaskGraph::Options o;
    o.workers = 2;
    o.shouldContinue = [&] { return keepGoing.load(); };
    std::thread killer([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        keepGoing = false;
    });
    const size_t drained = g.run(o);
    killer.join();

    CHECK(drained < 64, "stopped early (%zu of 64 drained)", drained);
    CHECK(doneCount.load() == (int)drained,
          "every dispatched task drained (%d == %zu)", doneCount.load(), drained);
    CHECK(started.load() >= (int)drained,
          "no task drained without running (%d started)", started.load());
}

// ── 5. Concurrent storeBytes ─────────────────────────────────────────────────
static void testDdcConcurrentStoreBytes(const fs::path& tmp) {
    std::printf("[ddc] concurrent same-key storeBytes\n");
    const fs::path root = tmp / "ddc-race";
    fs::remove_all(root);
    assetlib::DdcStore ddc(root, {});

    // 8 threads store the SAME key with the SAME payload — as two cooks of
    // duplicate assets would. With a pid-only temp name they collide on one
    // temp file and can ingest a torn blob.
    const std::string key(64, 'a');
    const std::string payload(256 * 1024, 'Z');
    std::atomic<int> ok{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; ++i)
        ts.emplace_back([&] { if (ddc.storeBytes(key, payload)) ++ok; });
    for (auto& t : ts) t.join();

    CHECK(ok.load() == 8, "all 8 writers succeeded (%d)", ok.load());
    std::string got;
    CHECK(ddc.fetchBytes(key, got), "blob readable after the race");
    CHECK(got == payload, "blob is intact, not torn (%zu of %zu bytes)",
          got.size(), payload.size());
    // No temp files may survive a clean run.
    int leftovers = 0;
    std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(root, ec))
        if (e.is_regular_file() &&
            e.path().string().find(".bytes.") != std::string::npos) ++leftovers;
    CHECK(leftovers == 0, "no temp files leaked (%d)", leftovers);
}

// ── 6. Hostile manifest names ────────────────────────────────────────────────
static void testManifestRejectsHostileNames(const fs::path& tmp) {
    std::printf("[ddc] manifest member-name validation\n");
    const fs::path root = tmp / "ddc-manifest";
    fs::remove_all(root);
    assetlib::DdcStore ddc(root, {});

    // A real member blob to point at, so only the NAME is under test.
    const fs::path victim = tmp / "member.bin";
    writeFile(victim, "payload");
    const std::string memberKey = assetlib::blake3File(victim);
    CHECK(!memberKey.empty() && ddc.store(memberKey, victim),
          "member blob stored");

    auto tryName = [&](const std::string& name, const char* label) {
        const std::string key(64, 'b');
        // Fresh store per case: keys are immutable once written.
        const fs::path r = tmp / ("mf-" + std::string(label));
        fs::remove_all(r);
        assetlib::DdcStore s(r, {});
        s.store(memberKey, victim);
        std::string manifest = "ddc-manifest-v1\n";
        manifest += memberKey + "\t@\n";          // valid primary
        manifest += memberKey + "\t" + name + "\n";
        s.storeBytes(key, manifest);
        const bool fetched = assetlib::ddcFetchRecord(s, key,
                                 tmp / "out" / "primary.cooked");
        CHECK(!fetched, "rejected member name %s", label);
    };
    tryName("../escape.ctex",   "parent-traversal");
    tryName("sub/dir.ctex",     "forward-slash");
    tryName("sub\\dir.ctex",    "backslash");
    tryName("C:evil.ctex",      "windows-drive-relative");
    tryName("/abs/evil.ctex",   "absolute");
    tryName("..",               "dotdot");

    // The honest case must still work.
    {
        const std::string key(64, 'c');
        const fs::path r = tmp / "mf-good";
        fs::remove_all(r);
        assetlib::DdcStore s(r, {});
        s.store(memberKey, victim);
        std::string manifest = "ddc-manifest-v1\n";
        manifest += memberKey + "\t@\n";
        manifest += memberKey + "\tmesh_t0.ctex\n";
        s.storeBytes(key, manifest);
        const fs::path out = tmp / "out2" / "primary.cooked";
        CHECK(assetlib::ddcFetchRecord(s, key, out),
              "accepted a plain sibling filename");
        CHECK(fs::exists(out.parent_path() / "mesh_t0.ctex"),
              "sibling member materialized beside the primary");
    }
}

// ── 7/8. Registry scan move detection ────────────────────────────────────────
static void testScanMoveDetection(const fs::path& tmp) {
    std::printf("[registry] scan move detection\n");
    const fs::path proj   = tmp / "proj";
    const fs::path assets = proj / "assets";
    fs::remove_all(proj);
    writeFile(assets / "a.png", "AAAA-content");
    writeFile(assets / "b.png", "BBBB-content");

    assetlib::AssetRegistry reg;
    CHECK(reg.open(proj / ".cache" / "registry.db"), "registry opened");
    reg.scan(assets, proj);
    auto recA = reg.findBySourcePath("assets/a.png");
    auto recB = reg.findBySourcePath("assets/b.png");
    CHECK(recA && recB, "both assets registered");
    const std::string uuidA = recA ? recA->uuid.toString() : "";

    // Rename a.png -> moved/a2.png. Same bytes, new path: the record must be
    // re-pointed, NOT replaced with a fresh UUID (scene refs key on UUID).
    fs::create_directories(assets / "moved");
    fs::rename(assets / "a.png", assets / "moved" / "a2.png");
    reg.scan(assets, proj);

    auto moved = reg.findBySourcePath("assets/moved/a2.png");
    CHECK(moved, "moved file is registered at its new path");
    CHECK(moved && moved->uuid.toString() == uuidA,
          "moved file kept its UUID (%s)",
          moved ? moved->uuid.toString().c_str() : "none");
    CHECK(!reg.findBySourcePath("assets/a.png").has_value()
          || reg.findBySourcePath("assets/a.png")->state
                 == assetlib::AssetState::Missing,
          "old path is gone or Missing");

    // Two files with IDENTICAL content, both deleted, then ONE re-added under
    // a new name: exactly one record may be claimed by the newcomer.
    writeFile(assets / "dup1.png", "SAME-BYTES");
    writeFile(assets / "dup2.png", "SAME-BYTES");
    reg.scan(assets, proj);
    const auto d1 = reg.findBySourcePath("assets/dup1.png");
    const auto d2 = reg.findBySourcePath("assets/dup2.png");
    CHECK(d1 && d2 && d1->uuid != d2->uuid,
          "identical-content files get distinct UUIDs");
    fs::remove(assets / "dup1.png");
    fs::remove(assets / "dup2.png");
    reg.scan(assets, proj);
    writeFile(assets / "dup3.png", "SAME-BYTES");
    reg.scan(assets, proj);
    const auto d3 = reg.findBySourcePath("assets/dup3.png");
    CHECK(d3, "re-added duplicate is registered");
    const bool claimedOne = d3 && (d3->uuid == d1->uuid || d3->uuid == d2->uuid);
    CHECK(claimedOne, "re-added file adopted one of the two dead records");
    // The OTHER record must still exist independently (not double-claimed).
    int stillMissing = 0;
    for (const auto& r : reg.all())
        if ((r.uuid == d1->uuid || r.uuid == d2->uuid)
                && r.uuid != d3->uuid) ++stillMissing;
    CHECK(stillMissing == 1, "the other record survives untouched (%d)",
          stillMissing);
    reg.close();
}

int main() {
    // Unbuffered: ctest redirects stdout, which makes it block-buffered,
    // and a test killed on timeout loses everything still in the buffer.
    // Unbuffered + a watchdog under ctest's 120 s: this test TIMES OUT on
    // Windows and reported only up to the hostile-manifest section, which with a
    // block-buffered stdout is not evidence of where it stopped. The phase
    // markers below name the last section that actually STARTED.
    testwd::begin("=== cook_infra_test ===", 60);
    const fs::path tmp = fs::temp_directory_path() / "engine-cook-infra-test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);

    testwd::phase("1/2 graph ordering");        testGraphOrdering();
    testwd::phase("3 graph cycle");             testGraphCycle();
    testwd::phase("4 graph cancel");            testGraphCancel();
    testwd::phase("5 ddc concurrent storeBytes"); testDdcConcurrentStoreBytes(tmp);
    testwd::phase("6 hostile manifest names");  testManifestRejectsHostileNames(tmp);
    testwd::phase("7/8 registry scan move detection"); testScanMoveDetection(tmp);
    testwd::phase("teardown");

    fs::remove_all(tmp, ec);
    testwd::end();
    if (g_failures) {
        std::printf("cook_infra_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("cook_infra_test: ALL PASS\n");
    return 0;
}
