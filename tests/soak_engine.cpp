// ── soak_engine — continuous-run endurance harness ───────────────────────────
// The bugs that survive every unit test: memory that grows a few KB per
// play session, a job queue that deadlocks once per million tasks, caches
// without bounds, NaN creep from float drift, thread races that need hours
// of interleavings. This harness runs the FULL engine (headless) through
// repeated play-session cycles — spawn/destroy churn, asset polls, job
// storms — sampling metrics per cycle and failing on TRENDS, not just
// crashes:
//   - live-memory slope after warmup  -> leak detection
//   - canary job round-trip watchdog  -> pool deadlock/starvation
//   - edit-world entity baseline      -> world-teardown leaks
//   - transform finiteness sweep      -> NaN/inf creep (FP instability)
//   - parallelFor exact coverage      -> lost ranges after millions of ops
//
//   soak_engine [minutes]      duration (default 1 — the CI smoke run)
//   SOAK_MINUTES=2880 …        env override: the real multi-day soak
//
// Run the TSan lane build for the sync-bug hunt; run this (Release) for
// trends. Exits non-zero on the first violated invariant.
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "runtime/runtime.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/jobs/jobs.h"
#include "core/memory/mem.h"

namespace { int g_failures = 0; }
#define REQUIRE(cond, ...) do {                                     \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
} while (0)

static uint64_t totalLiveBytes() {
    uint64_t sum = 0;
    for (int t = 0; t < (int)mem::Tag::Count; ++t)
        sum += mem::stats((mem::Tag)t).currentBytes;
    return sum;
}

// Least-squares slope of y over sample index (bytes per cycle).
static double slopePerCycle(const std::vector<uint64_t>& y) {
    const size_t n = y.size();
    if (n < 2) return 0.0;
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (size_t i = 0; i < n; ++i) {
        sx += (double)i; sy += (double)y[i];
        sxx += (double)i * (double)i; sxy += (double)i * (double)y[i];
    }
    const double denom = (double)n * sxx - sx * sx;
    return denom != 0.0 ? ((double)n * sxy - sx * sy) / denom : 0.0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    double minutes = 1.0;
    if (const char* env = std::getenv("SOAK_MINUTES")) minutes = std::atof(env);
    else if (argc > 1)                                 minutes = std::atof(argv[1]);
    if (minutes <= 0) minutes = 1.0;

    std::printf("soak_engine: %.1f minute endurance run\n", minutes);

    EngineConfig cfg;
    cfg.openAssetDatabase = false;   // no background DB churn — pure runtime
    EngineRuntime engine;
    if (!engine.init(cfg, std::make_unique<HeadlessPlatform>())) {
        std::printf("soak_engine: FAIL — init\n");
        return 1;
    }
    engine.attachPlugins();

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::duration<double>(minutes * 60.0);
    const float dt = 1.0f / 60.0f;

    std::vector<uint64_t> memSamples;
    int64_t  entityBaseline = -1;
    uint64_t cycles = 0, totalJobs = 0, totalEntities = 0;
    double   worstCanaryMs = 0.0;

    while (std::chrono::steady_clock::now() < deadline && g_failures == 0) {
        ++cycles;

        // ── Play session with churn ───────────────────────────────────────
        engine.startSimulation(EngineRuntime::SimMode::Snapshot);
        flecs::world& game = engine.simWorld();

        for (int tick = 0; tick < 120; ++tick) {          // ~2s of sim
            // Entity churn: a spawner-heavy game, compressed.
            std::vector<flecs::entity> spawned;
            spawned.reserve(16);
            for (int i = 0; i < 16; ++i) {
                Transform t{};
                t.position = {(float)(tick % 32), 1.0f, (float)i};
                t.rotation = {0, 0, 0, 1};
                t.scale    = {1, 1, 1};
                spawned.push_back(game.entity().set<Transform>(t));
                ++totalEntities;
            }
            engine.tick(dt);
            engine.tickSimulation(dt);
            for (auto& e : spawned) e.destruct();

            // Job storm: fire-and-forget batch + exact-coverage parallelFor.
            if (tick % 10 == 0) {
                std::atomic<int> ran{0};
                jobs::JobHandle h[8];
                for (int j = 0; j < 8; ++j)
                    h[j] = jobs::run("soak.storm", [&] { ++ran; });
                for (int j = 0; j < 8; ++j) jobs::wait(h[j]);
                REQUIRE(ran.load() == 8, "job storm ran 8/8 (%d)", ran.load());
                totalJobs += 8;

                std::atomic<uint64_t> sum{0};
                jobs::parallelFor("soak.pfor", 10000, 256,
                    [&](uint32_t b, uint32_t e2) {
                        uint64_t local = 0;
                        for (uint32_t i = b; i < e2; ++i) local += i;
                        sum.fetch_add(local, std::memory_order_relaxed);
                    });
                REQUIRE(sum.load() == 10000ull * 9999ull / 2ull,
                        "parallelFor exact coverage (cycle %" PRIu64 ")", cycles);
            }
        }

        // ── FP sanity: no NaN/inf creep anywhere in the sim world ────────
        int nonFinite = 0;
        game.each([&](flecs::entity, Transform& t) {
            const float* f = &t.position.x;
            for (int i = 0; i < 3; ++i)
                if (!std::isfinite(f[i])) ++nonFinite;
            if (!std::isfinite(t.rotation.x) || !std::isfinite(t.rotation.w))
                ++nonFinite;
        });
        REQUIRE(nonFinite == 0, "no NaN/inf transforms (cycle %" PRIu64 ", %d bad)",
                cycles, nonFinite);

        engine.stopSimulation();

        // ── Canary: the pool must still respond promptly ─────────────────
        {
            const auto t0 = std::chrono::steady_clock::now();
            std::atomic<bool> alive{false};
            jobs::JobHandle canary = jobs::run("soak.canary", [&] { alive = true; });
            jobs::wait(canary);
            const double ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0).count();
            worstCanaryMs = std::max(worstCanaryMs, ms);
            REQUIRE(alive.load(), "canary job executed (cycle %" PRIu64 ")", cycles);
            REQUIRE(ms < 10000.0, "pool responsive: canary %.1fms (deadlock?)", ms);
        }

        // ── Edit-world baseline: sessions must not leak entities ─────────
        int64_t edit = 0;
        engine.simWorld().each([&](flecs::entity, Transform&) { ++edit; });
        if (entityBaseline < 0) entityBaseline = edit;
        REQUIRE(edit == entityBaseline,
                "edit world back to baseline (%lld vs %lld, cycle %" PRIu64 ")",
                (long long)edit, (long long)entityBaseline, cycles);

        memSamples.push_back(totalLiveBytes());
        if (cycles % 10 == 1)
            std::printf("  cycle %-5" PRIu64 " live=%.2f MB canary=%.2fms "
                        "entities=%" PRIu64 " jobs=%" PRIu64 "\n",
                        cycles, memSamples.back() / (1024.0 * 1024.0),
                        worstCanaryMs, totalEntities, totalJobs);
    }

    // ── Trend verdicts ───────────────────────────────────────────────────
    std::printf("soak_engine: %" PRIu64 " cycles, %" PRIu64 " entities churned, "
                "%" PRIu64 " jobs, worst canary %.2fms\n",
                cycles, totalEntities, totalJobs, worstCanaryMs);

    if (memSamples.size() >= 8) {
        // Skip the first quarter: caches legitimately warm up (flecs tables,
        // TLSF pools, query caches). After warmup the slope must be flat.
        const size_t skip = memSamples.size() / 4;
        std::vector<uint64_t> settled(memSamples.begin() + skip, memSamples.end());
        const double slope = slopePerCycle(settled);
        const double total = slope * (double)settled.size();
        std::printf("  memory: %.2f MB live, slope %+.1f KB/cycle "
                    "(%+.2f MB over %zu settled cycles)\n",
                    memSamples.back() / (1024.0 * 1024.0), slope / 1024.0,
                    total / (1024.0 * 1024.0), settled.size());
        // A real leak compounds: sustained positive slope AND material total.
        REQUIRE(!(slope > 64.0 * 1024.0 && total > 8.0 * 1024.0 * 1024.0),
                "memory slope indicates a leak (%.1f KB/cycle)", slope / 1024.0);
    } else {
        std::printf("  memory: %.2f MB live (too few cycles for trend — "
                    "run longer for leak detection)\n",
                    memSamples.empty() ? 0.0
                    : memSamples.back() / (1024.0 * 1024.0));
    }

    engine.shutdown();

    if (g_failures) { std::printf("soak_engine: %d FAILURE(S)\n", g_failures); return 1; }
    std::printf("soak_engine: SURVIVED\n");
    return 0;
}
