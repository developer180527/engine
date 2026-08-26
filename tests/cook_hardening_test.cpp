// ── cook_hardening_test — the reliability fixes from the DDC/cook audit ──────
//
// Four independent failure modes, each of which was silent before:
//
//   1. RESULT-FILE FRAMING. `RESULT ok` is the first body line a cook worker
//      writes, so a worker killed part-way through (deadline SIGKILL, rlimit
//      OOM, signal from a corrupt parse) left a file that parsed as a clean
//      SUCCESS with its OUTPUT lines simply absent — and the asset committed
//      without its sibling textures. That is the silently-untextured build,
//      arriving through the IPC channel rather than the packager.
//
//   2. DDC EVICTION. Keys are derived from inputs, so every source edit, cooker
//      version bump or settings change mints a NEW key and orphans the old blob
//      with no referrer left to notice. Nothing collected them; the store grew
//      without bound forever.
//
//   3. REGISTRY SCHEMA VERSIONING. Every migration error was discarded and
//      open() returned true regardless, so a database written by a NEWER build
//      opened "successfully" and then silently dropped columns it could not see
//      on every write.
//
//   4. MOVE DETECTION. A renamed source must keep its UUID or every scene
//      reference to it silently unlinks. Nothing tested it, and a scan change
//      that re-pointed the record and then marked it Missing in the same pass
//      passed the entire suite.
//
// Hermetic: no GPU, no project, no cooker. Temp dirs only.
#include <cstring>   // std::memcpy/std::strlen — libc++ pulls these in
                     // transitively, libstdc++ does not, so the Linux legs
                     // are where a missing one surfaces
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <engine/addon_protocol.h>

#include <assetlib/asset_registry.h>
#include <assetlib/cook_result_file.h>
#include <assetlib/ddc.h>
#include <chrono>

namespace fs = std::filesystem;
namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);             \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__);  \
                   ++g_failures; }                                   \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

using namespace assetlib;

// ── 1. Result-file framing ──────────────────────────────────────────────────
static void testResultFraming() {
    std::printf("\n-- result file framing --\n");

    // The shape a healthy worker writes: a verdict plus two sibling outputs.
    std::string body = "RESULT ok\nOUTPUT /tmp/a_t0.ctex\nOUTPUT /tmp/a_t1.ctex\n";
    const std::string good = cookresult::frame(body, 3);

    std::string out, err;
    CHECK(cookresult::unframe(good, out, err), "a complete frame validates");
    CHECK(out == body, "...and round-trips the body byte for byte");

    // THE BUG. Truncate after the verdict line — exactly what a SIGKILL between
    // write() calls produces. The old parser read this as success with no
    // outputs; it must now be rejected outright.
    const size_t afterVerdict = good.find("RESULT ok\n") + 10;
    const std::string cut = good.substr(0, afterVerdict);
    CHECK(cut.find("RESULT ok") != std::string::npos,
          "the truncated file DOES still contain a clean \"RESULT ok\"");
    CHECK(!cookresult::unframe(cut, out, err),
          "...and is rejected anyway: %s", err.c_str());

    // Truncated mid-line — the other common shape.
    CHECK(!cookresult::unframe(good.substr(0, good.size() - 4), out, err),
          "a file cut mid-trailer is rejected: %s", err.c_str());

    // A body edited without updating the trailer: line count still matches, so
    // only the digest can catch it.
    std::string tampered = good;
    tampered.replace(tampered.find("a_t0"), 4, "b_t9");
    CHECK(!cookresult::unframe(tampered, out, err),
          "a body whose digest disagrees with END is rejected: %s", err.c_str());

    // An OUTPUT line dropped: the line count catches this one.
    std::string dropped = cookresult::frame(body, 3);
    const size_t o1 = dropped.find("OUTPUT /tmp/a_t1.ctex\n");
    dropped.erase(o1, std::strlen("OUTPUT /tmp/a_t1.ctex\n"));
    CHECK(!cookresult::unframe(dropped, out, err),
          "a body missing a line is rejected: %s", err.c_str());

    // Not one of ours at all, and the empty file.
    CHECK(!cookresult::unframe("RESULT ok\n", out, err),
          "an unframed legacy file is rejected: %s", err.c_str());
    CHECK(!cookresult::unframe("", out, err), "the empty file is rejected");

    // A zero-output success must still work — the frame must not require a body
    // beyond the verdict, or every texture cook starts failing.
    const std::string minimal = cookresult::frame("RESULT ok\n", 1);
    CHECK(cookresult::unframe(minimal, out, err) && out == "RESULT ok\n",
          "a verdict-only result validates");

    // ── The same framing, in two places, kept honest ────────────────────────
    // `engine/addon_protocol.h` implements this identical frame under its own
    // magic, and is deliberately NOT expressed in terms of this header: assetlib
    // is a standalone CMake project that does not depend on the engine SDK, and
    // inverting that dependency to remove thirty lines would be the worse trade.
    //
    // Duplication that nothing checks is duplication that drifts. Change the
    // digest seed here, the trailer spelling, the version position, the newline
    // handling — every one of those keeps both files compiling and passing their
    // own tests while the two formats silently stop being the same format. Then
    // the day one is asked to read the other's file, or a host is taught "the
    // trailer looks like this", it is wrong in a way no test wrote down.
    //
    // So: frame the same body through both and require identical bytes below the
    // magic. Cheap, and it converts "keep these in sync" from a comment into a
    // build failure.
    {
        const std::string sameBody = "VERDICT ok\nMODULE 0 ok /tmp/x.so\n";
        const std::string viaCook  = cookresult::frame(sameBody, 2);
        const std::string viaAddon =
            engine::addon::frame(cookresult::kMagic, sameBody, 2);
        CHECK(viaCook == viaAddon,
              "the cook and add-on framings agree byte for byte on one body");

        CHECK(engine::addon::fnv1a64(sameBody) == cookresult::fnv1a64(sameBody),
              "...and on the digest, which is the half a length check cannot see");

        // Version numbers are independent by design — the cook result file and
        // the Add-on protocol may be revised separately — so this is the one
        // place the two are allowed to differ, and it is asserted rather than
        // assumed, because the check above would start failing for this reason
        // and the reason should be written down before it does.
        CHECK(cookresult::kVersion == engine::addon::kProtocolVersion,
              "both formats are still at version %d; when one is bumped, the "
              "byte-for-byte check above must take a version argument rather "
              "than being deleted", cookresult::kVersion);
    }
}

// ── 2. DDC eviction ─────────────────────────────────────────────────────────
static void testDdcEviction() {
    std::printf("\n-- DDC budget + LRU eviction --\n");

    const fs::path root = fs::temp_directory_path() / "engine_ddc_gc_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path local = root / "local";
    const fs::path work  = root / "work";
    fs::create_directories(work, ec);

    DdcStore store(local, {});          // local tier only

    // Four blobs of 4 KiB each, stored oldest-first with a gap between them so
    // mtime ordering is unambiguous on filesystems with coarse timestamps.
    std::vector<std::string> keys;
    for (int i = 0; i < 4; ++i) {
        const fs::path src = work / ("blob" + std::to_string(i) + ".bin");
        { std::ofstream f(src, std::ios::binary);
          f << std::string(4096, char('a' + i)); }
        const std::string key(64, char('0' + i));      // 64-hex-ish, fans out fine
        CHECK(store.store(key, src), "stored blob %d", i);
        keys.push_back(key);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    // Budget above the total: nothing to do, and nothing deleted.
    auto st = store.collectGarbage(1u << 20, /*prune*/true);
    CHECK(st.blobs == 4, "sees all 4 blobs (%llu)", (unsigned long long)st.blobs);
    CHECK(st.totalBytes >= 4 * 4096, "totals their bytes (%llu)",
          (unsigned long long)st.totalBytes);
    CHECK(st.deleted == 0, "under budget deletes nothing");

    // Touch blob 0 via a fetch, making it the most-recently-USED even though it
    // is the oldest by ingest. This is the property that makes the policy LRU
    // rather than FIFO.
    const fs::path dst = work / "fetched.bin";
    CHECK(store.fetch(keys[0], dst), "fetched blob 0 (marks it recently used)");
    fs::remove(dst, ec);                // drop the hardlink so it is not pinned

    // Dry run first: reports the overage without touching anything.
    st = store.collectGarbage(8192, /*prune*/false);
    CHECK(st.overBudgetBytes > 0, "dry run reports being over budget");
    CHECK(st.deleted == 0 && st.freedBytes == 0,
          "dry run deletes nothing");
    CHECK(store.contains(keys[1]), "...and every blob survives a dry run");

    // Now evict down to 8 KiB — two of four blobs must go, and blob 0 must not
    // be among them because it was just used.
    st = store.collectGarbage(8192, /*prune*/true);
    CHECK(st.deleted == 2, "evicted 2 blob(s) to reach budget (%llu)",
          (unsigned long long)st.deleted);
    CHECK(st.freedBytes >= 8192, "freed their bytes (%llu)",
          (unsigned long long)st.freedBytes);
    CHECK(store.contains(keys[0]),
          "the recently USED blob survived (LRU, not FIFO)");
    CHECK(store.contains(keys[3]), "the newest blob survived");
    CHECK(!store.contains(keys[1]) && !store.contains(keys[2]),
          "the two least-recently-used blobs are gone");

    // Pinning: a blob hardlinked into a live .cache must not be counted as
    // reclaimable, because unlinking the store's copy frees zero bytes.
    const fs::path pinned = work / "pinned.bin";
    CHECK(store.fetch(keys[0], pinned), "materialized blob 0 into a 'project'");
    st = store.collectGarbage(0 /*unbounded*/, false);
    const bool hardlinked = fs::hard_link_count(pinned, ec) > 1;
    if (hardlinked) {
        CHECK(st.pinnedBytes >= 4096,
              "a hardlinked blob counts as PINNED, not reclaimable (%llu B)",
              (unsigned long long)st.pinnedBytes);
    } else {
        // Materialization falls back to a copy across filesystems; then there
        // is no pinning to observe and the check would be vacuous.
        std::printf("  skip  pinning: materialize fell back to copy "
                    "(no hardlink on this filesystem)\n");
    }

    // A zero budget means unbounded, NOT "evict everything" — the difference
    // between an opt-in policy and a cache that deletes itself by default.
    st = store.collectGarbage(0, true);
    CHECK(st.deleted == 0, "budget 0 means unbounded, and evicts nothing");
    CHECK(store.contains(keys[0]), "...the store is intact");

    fs::remove_all(root, ec);
}

// ── 3. Registry schema versioning ───────────────────────────────────────────
static void testSchemaVersion() {
    std::printf("\n-- registry schema versioning --\n");

    const fs::path dir = fs::temp_directory_path() / "engine_registry_ver_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path dbPath = dir / "registry.db";

    {
        AssetRegistry reg;
        CHECK(reg.open(dbPath), "a fresh registry opens");
    }
    {   // Re-opening runs every additive ALTER again; they all fail benignly as
        // "duplicate column name" and must NOT be reported as a failure.
        AssetRegistry reg;
        CHECK(reg.open(dbPath), "re-opening an existing registry still succeeds");
    }

    // Forge a database claiming a FUTURE schema. This build must refuse it
    // rather than write to it and drop the columns it cannot see.
    //
    // user_version lives at bytes 60..63 of the SQLite header, big-endian — a
    // stable part of the documented on-disk format. Patching it directly keeps
    // this test free of a direct sqlite3 dependency and avoids widening
    // AssetRegistry's API (execSQL is private, correctly) just to test this.
    // The -wal/-shm go too, so page 1 cannot be re-read from a stale WAL.
    fs::remove(dbPath.string() + "-wal", ec);
    fs::remove(dbPath.string() + "-shm", ec);
    {
        std::fstream f(dbPath, std::ios::binary | std::ios::in | std::ios::out);
        CHECK(f.good(), "opened the database file to forge its version");
        const unsigned char future[4] = { 0x00, 0x00, 0x27, 0x0F };   // 9999
        f.seekp(60);
        f.write(reinterpret_cast<const char*>(future), 4);
        f.close();
        CHECK(f.good(), "marked the database as written by a newer build");
    }
    {
        AssetRegistry reg;
        CHECK(!reg.open(dbPath),
              "a registry from a NEWER build is refused, not silently downgraded");
    }

    fs::remove_all(dir, ec);
}

// ── 4. Move detection ───────────────────────────────────────────────────────
// A renamed source file must keep its UUID, because scenes reference assets BY
// UUID — losing it silently unlinks every reference to that asset. This is the
// scan's most easily-broken invariant: it needs the record's re-pointed
// sourcePath to be visible to the "mark absent files Missing" sweep that runs
// immediately after, so any change that makes that sweep read a STALE snapshot
// re-points the asset correctly and then marks it Missing in the same pass.
// (That is not hypothetical — it was written, and no existing test caught it.)
static void testMoveDetection() {
    std::printf("\n-- move detection: a rename keeps the UUID --\n");

    const fs::path root = fs::temp_directory_path() / "engine_scan_move_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path assets = root / "assets";
    fs::create_directories(assets, ec);

    const fs::path before = assets / "before.png";
    { std::ofstream f(before, std::ios::binary);
      f << "not really a png, but it hashes just fine"; }

    AssetRegistry reg;
    CHECK(reg.open(root / ".cache" / "registry.db"), "registry opens");
    reg.scan(assets, root);

    auto rec = reg.findBySourcePath("assets/before.png");
    CHECK(rec.has_value(), "the file registered");
    if (!rec) return;
    const std::string uuid = rec->uuid.toString();

    fs::rename(before, assets / "after.png", ec);
    CHECK(!ec, "renamed on disk");
    reg.scan(assets, root);

    const auto moved = reg.findBySourcePath("assets/after.png");
    CHECK(moved.has_value(), "the new path resolves");
    CHECK(moved && moved->uuid.toString() == uuid,
          "...to the SAME uuid (scene references survive a rename)");
    CHECK(moved && moved->state != AssetState::Missing,
          "...and it is NOT marked Missing by the sweep that follows the move");
    CHECK(!reg.findBySourcePath("assets/before.png").has_value()
          || reg.findBySourcePath("assets/before.png")->uuid.toString() == uuid,
          "the old path does not survive as a second record");

    fs::remove_all(root, ec);
}

// ── 5. Multi-root scans share one registry ──────────────────────────────────
// CookService scans the PROJECT's assets and the ENGINE's own defaults against
// the same projectRoot, so both sets of records live in one registry. The
// "mark absent files Missing" sweep used to iterate every record regardless of
// which root was just walked, so each scan declared the other root's assets
// deleted: the project scan marked every engine shader Missing, the engine scan
// marked every project asset Missing, and a plain editor boot (which scans only
// the project) marked all the engine defaults Missing. Measured on fps_shooter
// before the fix: 653 of 653 records were Missing — the whole registry.
static void testMultiRootScan() {
    std::printf("\n-- two asset roots, one registry --\n");

    const fs::path root = fs::temp_directory_path() / "engine_multiroot_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const fs::path projAssets = root / "assets";
    const fs::path engAssets  = root / "engine_defaults";
    fs::create_directories(projAssets, ec);
    fs::create_directories(engAssets, ec);
    { std::ofstream f(projAssets / "game.png", std::ios::binary); f << "project asset"; }
    { std::ofstream f(engAssets  / "stock.png", std::ios::binary); f << "engine asset"; }

    AssetRegistry reg;
    CHECK(reg.open(root / ".cache" / "registry.db"), "registry opens");

    reg.scan(projAssets, root);   // project root
    reg.scan(engAssets,  root);   // engine defaults, SAME registry

    auto game  = reg.findBySourcePath("assets/game.png");
    auto stock = reg.findBySourcePath("engine_defaults/stock.png");
    CHECK(game && stock, "both roots registered their asset");
    if (!game || !stock) return;
    CHECK(game->state != AssetState::Missing,
          "the project asset is not Missing after the ENGINE root was scanned");
    CHECK(stock->state != AssetState::Missing,
          "the engine asset is not Missing after the PROJECT root was scanned");

    // Scanning only one root, repeatedly, must never touch the other's records.
    reg.scan(projAssets, root);
    CHECK(reg.findBySourcePath("engine_defaults/stock.png")->state
              != AssetState::Missing,
          "...and a project-only re-scan still leaves the engine asset alone");

    // But a real deletion under the scanned root MUST still be caught, or the
    // scoping has simply disabled the sweep.
    fs::remove(projAssets / "game.png", ec);
    reg.scan(projAssets, root);
    CHECK(reg.findBySourcePath("assets/game.png")->state == AssetState::Missing,
          "a genuinely deleted file under the scanned root IS marked Missing");
    CHECK(reg.findBySourcePath("engine_defaults/stock.png")->state
              != AssetState::Missing,
          "...and the other root is still untouched");

    // Healing: a record wrongly left Missing by an older build must recover the
    // moment a scan sees the file present again.
    { std::ofstream f(projAssets / "game.png", std::ios::binary); f << "project asset"; }
    reg.scan(projAssets, root);
    CHECK(reg.findBySourcePath("assets/game.png")->state != AssetState::Missing,
          "a Missing record heals when the file is present again");

    fs::remove_all(root, ec);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("cook_hardening_test: DDC/cook audit reliability fixes\n");
    testResultFraming();
    testDdcEviction();
    testSchemaVersion();
    testMoveDetection();
    testMultiRootScan();

    if (g_failures) {
        std::printf("\ncook_hardening_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\ncook_hardening_test: all checks passed\n");
    return 0;
}
