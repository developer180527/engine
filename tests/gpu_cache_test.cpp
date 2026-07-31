// ── gpu_cache_test — identity, refcounts and budget for GPU resources ───────
//
// Phase 1 of docs/renderer-audit-and-plan.md. The audit's finding R1 was that
// GPU resources have no identity, no refcount and no dedup, which is why
// duplicates cannot be prevented, leaks cannot be defined, and the render
// tooling cannot be written at all.
//
// The properties asserted here are exactly the ones those tools stand on:
//   • the same content key NEVER uploads twice          (duplicate report)
//   • refs == 0 is the definition of unused             (leak detector)
//   • a referenced resource is NEVER evicted            (no use-after-free)
//   • eviction is LRU and respects a byte budget        (128 MB target)
//   • the census accounts for every resident byte       (VRAM census)
//
// Runs with a FAKE handle type and no bgfx, because GpuResourceCache is
// payload-agnostic by design — src/render has no GPU test harness and this
// component does not need one.
#include <cstdio>
#include <string>
#include <vector>

#include "render/gpu_resource_cache.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

// Stands in for a bgfx::TextureHandle: an opaque id the cache never inspects.
struct FakeHandle { uint32_t id = 0; };

using Cache = gpucache::GpuResourceCache<FakeHandle>;

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("gpu_cache_test: GPU resource identity + budget\n");

    // Records what the cache actually created and destroyed, so the test can
    // assert on real GPU-side effects rather than on the cache's own bookkeeping.
    uint32_t nextId = 1;
    int      creates = 0;
    std::vector<uint32_t> destroyed;

    auto destroyer = [&](const FakeHandle& h) { destroyed.push_back(h.id); };
    auto makeFactory = [&](size_t bytes) {
        return [&, bytes](FakeHandle& out, size_t& outBytes) {
            out.id = nextId++;
            outBytes = bytes;
            ++creates;
            return true;
        };
    };

    // ── 1. Dedup is structural: the same key never uploads twice ────────────
    {
        Cache c(destroyer);
        FakeHandle a{}, b{};
        CHECK(c.acquire("hashA", "rock_albedo.png", makeFactory(1000), a),
              "first acquire succeeds");
        CHECK(c.acquire("hashA", "rock_albedo_COPY.png", makeFactory(1000), b),
              "second acquire of the SAME CONTENT succeeds");
        CHECK(a.id == b.id, "both callers got the SAME handle (%u == %u)", a.id, b.id);
        CHECK(creates == 1, "the factory ran ONCE — no duplicate upload (%d)", creates);
        CHECK(c.refCount("hashA") == 2, "refcount tracks both holders (%u)",
              c.refCount("hashA"));
        CHECK(c.stats().liveBytes == 1000,
              "one payload accounted, not two (%zu B)", c.stats().liveBytes);
        CHECK(c.dedupSavedBytes() == 1000,
              "the avoided upload is MEASURED, not assumed (%zu B)",
              c.dedupSavedBytes());
    }

    // Two different files with identical bytes are the real-world case: the
    // pistol and the crate both shipping the same 4K normal map.
    {
        creates = 0;
        Cache c(destroyer);
        FakeHandle a{}, b{};
        c.acquire("sha_same", "packA/normal.png", makeFactory(4 * 1024 * 1024), a);
        c.acquire("sha_same", "packB/normal.png", makeFactory(4 * 1024 * 1024), b);
        CHECK(creates == 1 && c.stats().liveBytes == 4u * 1024 * 1024,
              "identical bytes under two paths share one 4 MB upload");
    }

    // ── 2. A referenced resource is NEVER evicted ───────────────────────────
    {
        creates = 0; destroyed.clear();
        Cache c(destroyer, /*budget*/ 1000);   // deliberately tiny
        FakeHandle h{};
        c.acquire("big", "held.png", makeFactory(5000), h);   // 5x over budget
        CHECK(c.overBudget(), "cache reports itself over budget");
        const size_t freed = c.evictOverBudget();
        CHECK(freed == 0 && destroyed.empty(),
              "referenced resource survives eviction (freed %zu) — evicting it "
              "would be a use-after-free", freed);
        CHECK(c.overBudget(),
              "still over budget, and says so rather than lying by evicting");

        // Once released it becomes a candidate, and only then.
        CHECK(c.release("big"), "release drops the reference");
        CHECK(c.refCount("big") == 0, "refcount is now zero");
        CHECK(c.contains("big"),
              "resource STAYS RESIDENT at refs=0 — a cache, not a unique_ptr");
        CHECK(c.evictOverBudget() == 5000 && destroyed.size() == 1,
              "now it evicts, and the destroyer actually ran");
        CHECK(!c.contains("big"), "evicted entry is gone from the table");
    }

    // ── 3. Eviction order is least-recently-used ────────────────────────────
    {
        creates = 0; destroyed.clear();
        Cache c(destroyer, 0);
        FakeHandle h{};
        c.acquire("old",  "a.png", makeFactory(100), h); const uint32_t oldId = h.id;
        c.acquire("mid",  "b.png", makeFactory(100), h);
        c.acquire("new",  "c.png", makeFactory(100), h); const uint32_t newId = h.id;
        c.release("old"); c.release("mid"); c.release("new");

        // Touch "old" so it is no longer the oldest — LRU must follow USE, not
        // insertion, or a hot resource gets evicted every frame.
        c.addRef("old"); c.release("old");

        c.setBudget(250);                 // must free ~1 entry
        c.evictOverBudget();
        CHECK(destroyed.size() == 1, "exactly one entry evicted (%zu)", destroyed.size());
        CHECK(destroyed[0] != oldId,
              "the re-touched entry was NOT the victim (LRU follows use)");
        CHECK(c.contains("old") && c.contains("new") && !c.contains("mid"),
              "the genuinely least-recently-used entry went");
        (void)newId;
    }

    // ── 4. Leak detection: what teardown must leave behind ──────────────────
    {
        creates = 0; destroyed.clear();
        Cache c(destroyer, 0);
        FakeHandle h{};
        c.acquire("t1", "level1/wall.png",  makeFactory(2000), h);
        c.acquire("t2", "level1/floor.png", makeFactory(3000), h);
        const size_t loaded = c.stats().liveBytes;
        CHECK(loaded == 5000, "scene resident set is %zu B", loaded);

        // Correct teardown: every acquire is matched by a release.
        c.release("t1"); c.release("t2");
        c.evictAllUnreferenced();
        CHECK(c.stats().liveBytes == 0,
              "clean unload returns to baseline (%zu B) — this is the leak "
              "assertion the soak lane will make", c.stats().liveBytes);
        CHECK(c.stillReferenced().empty(), "nothing still referenced");
    }
    {
        // The failing case the detector must catch: someone forgot to release.
        Cache c(destroyer, 0);
        FakeHandle h{};
        c.acquire("kept", "hud/crosshair.png", makeFactory(777), h);
        c.acquire("gone", "level1/wall.png",   makeFactory(1000), h);
        c.release("gone");
        c.evictAllUnreferenced();

        const auto leaked = c.stillReferenced();
        CHECK(leaked.size() == 1 && leaked[0].key == "kept",
              "leak report names the unreleased resource (%zu found)", leaked.size());
        CHECK(leaked[0].owner == "hud/crosshair.png" && leaked[0].bytes == 777,
              "and reports WHO owns it and how much it costs: %s, %zu B",
              leaked[0].owner.c_str(), leaked[0].bytes);
    }

    // ── 5. Census: every resident byte is attributable, biggest first ───────
    {
        Cache c(destroyer, 0);
        FakeHandle h{};
        c.acquire("small", "ui/icon.png",     makeFactory(64), h);
        c.acquire("huge",  "pistol_diff_4k",  makeFactory(11 * 1024 * 1024), h);
        c.acquire("mid",   "warehouse_1k",    makeFactory(1024 * 1024), h);

        const auto rows = c.census();
        CHECK(rows.size() == 3, "census lists every resident resource (%zu)", rows.size());
        CHECK(rows[0].key == "huge" && rows[1].key == "mid" && rows[2].key == "small",
              "sorted by cost, biggest first — the profiling view");
        size_t total = 0;
        for (const auto& r : rows) total += r.bytes;
        CHECK(total == c.stats().liveBytes,
              "census accounts for exactly the live byte total (%zu)", total);
        CHECK(rows[0].owner == "pistol_diff_4k",
              "each row names its owner, so a 11 MB texture is attributable");
    }

    // ── 6. Misuse is reported, not silently absorbed ────────────────────────
    {
        Cache c(destroyer, 0);
        FakeHandle h{};
        c.acquire("x", "a.png", makeFactory(10), h);
        CHECK(c.release("x"), "first release ok");
        CHECK(!c.release("x"),
              "DOUBLE RELEASE is rejected, not an underflow to 4 billion");
        CHECK(!c.release("never-existed"), "releasing an unknown key is rejected");

        auto failing = [](FakeHandle&, size_t&) { return false; };
        FakeHandle bad{};
        CHECK(!c.acquire("y", "broken.png", failing, bad),
              "a failed factory fails the acquire");
        CHECK(!c.contains("y"), "and leaves NO half-built entry behind");
    }

    if (g_failures) {
        std::printf("gpu_cache_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("gpu_cache_test: PASS\n");
    return 0;
}
