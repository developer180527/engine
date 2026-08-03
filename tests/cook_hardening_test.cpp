// ── cook_hardening_test — the reliability fixes from the DDC/cook audit ──────
//
// Three independent failure modes, each of which was silent before:
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
// Hermetic: no GPU, no project, no cooker. Temp dirs only.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <assetlib/asset_registry.h>
#include <assetlib/cook_result_file.h>
#include <assetlib/ddc.h>

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

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("cook_hardening_test: DDC/cook audit reliability fixes\n");
    testResultFraming();
    testDdcEviction();
    testSchemaVersion();

    if (g_failures) {
        std::printf("\ncook_hardening_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\ncook_hardening_test: all checks passed\n");
    return 0;
}
