// ── render_diag_test — the render diagnostics, headlessly ───────────────────
//
// Phase 2 of docs/plans/renderer-audit-and-plan.md. `src/render` is the least
// verified subsystem in the engine (tier `prototype`, no test that can fail)
// precisely because rendering needs a GPU. These modules are deliberately
// built so the parts worth testing DON'T:
//
//   frame_gpu_stats  — churn logic, fed through an explicit test seam
//   gpu_budget       — pure policy arithmetic
//   resource_census  — accounting over a payload-agnostic cache
//
// What is asserted is what the tools promise: churn is distinguished from
// steady state, a budget says PASS/OVER against real target numbers, a census
// attributes every byte to an owner, and a leak is detected by name and cost.
#include <cstdio>
#include <string>

#include "render/diag/frame_gpu_stats.h"
#include "render/diag/gpu_budget.h"
#include "render/diag/resource_census.h"
#include "render/gpu_resource_cache.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

using namespace rdiag;

static constexpr int64_t MB = 1024 * 1024;

// Feeds `frames` samples with a constant handle count.
static void steadyFrames(FrameGpuStats& s, int frames, uint16_t textures = 10) {
    HandleCounts c; c.textures = textures; c.programs = 14;
    for (int i = 0; i < frames; ++i)
        s.sampleExplicit(c, /*draws*/13, /*tex*/40 * MB, /*rt*/16 * MB, 200, 0);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("render_diag_test: render diagnostics (headless)\n");

    // ── 1. Churn: the diagnostic the tooling exists for ─────────────────────
    {
        FrameGpuStats s;
        CHECK(s.churn() == ChurnVerdict::NoData, "no samples -> NoData");

        steadyFrames(s, 600);
        CHECK(s.churn() == ChurnVerdict::Steady,
              "600 identical frames -> Steady (%s)", toString(s.churn()));
        CHECK(s.frames() == 600 && s.churnFrames() == 0,
              "600 frames, 0 churn frames (%llu)",
              (unsigned long long)s.churnFrames());
        CHECK(s.avgDraws() > 12.9 && s.avgDraws() < 13.1,
              "avg draws %.1f", s.avgDraws());
    }
    {
        // Create+destroy every frame: counts oscillate, total does not trend.
        FrameGpuStats s;
        HandleCounts a; a.textures = 10;
        HandleCounts b; b.textures = 11;
        for (int i = 0; i < 100; ++i)
            s.sampleExplicit((i % 2) ? a : b, 13, 0, 0, 0, 0);
        CHECK(s.churn() == ChurnVerdict::PerFrameChurn,
              "oscillating counts -> PerFrameChurn (%s)", toString(s.churn()));
    }
    {
        // Monotone growth: the leak signature, and it must outrank churn.
        FrameGpuStats s;
        HandleCounts c;
        for (int i = 0; i < 100; ++i) {
            c.textures = (uint16_t)(10 + i);
            s.sampleExplicit(c, 13, 0, 0, 0, 0);
        }
        CHECK(s.churn() == ChurnVerdict::LeakSuspected,
              "rising counts -> LeakSuspected (%s)", toString(s.churn()));
        CHECK(s.handleDrift() > 8, "upward drift recorded (%d)", s.handleDrift());
    }
    {
        // One resize in an otherwise steady run is expected, not a defect.
        FrameGpuStats s;
        HandleCounts a; a.textures = 10;
        HandleCounts b; b.textures = 12;
        for (int i = 0; i < 200; ++i) s.sampleExplicit(i == 100 ? b : a, 13, 0, 0, 0, 0);
        CHECK(s.churn() == ChurnVerdict::Occasional,
              "a single blip in 200 frames -> Occasional (%s)", toString(s.churn()));
    }

    // ── 2. Budget: measurement becomes a verdict ────────────────────────────
    {
        // The real shipped fps_shooter figures, and the guidance they imply.
        //
        // With the DEFAULT 2048 shadow map the frame is 23 MB of render
        // targets (16 MB shadow + 7 MB scene FB). That is comfortably inside
        // the 128 MB total, but OVER the 20 MB render-target sub-ceiling — and
        // that is the budget working as intended: the sub-limits are what push
        // a low-end project to 1024 shadows instead of quietly spending the
        // whole allowance on one allocation.
        FrameGpuStats s;
        HandleCounts c; c.textures = 6;
        s.sampleExplicit(c, 13, /*tex*/0, /*rt*/23 * MB, 0, 0);

        const auto r = evaluate(s, TargetTier::Low);
        CHECK(r.lines.size() == 4, "four budget lines (%zu)", r.lines.size());
        CHECK(!r.pass,
              "shipped default (2048 shadow, 23 MB rt) is OVER the low tier's "
              "render-target sub-ceiling — the signal to drop to 1024");
        CHECK(r.lines[0].ok,
              "…while TOTAL vram is fine (%.0f%% of 128 MB)",
              r.lines[0].fraction() * 100.0);

        // shadowResolution 1024 -> 4 MB shadow + 7 MB scene FB = 11 MB.
        FrameGpuStats low;
        low.sampleExplicit(c, 13, 0, 11 * MB, 0, 0);
        CHECK(evaluate(low, TargetTier::Low).pass,
              "shadowResolution 1024 (11 MB rt) PASSES the low tier outright");

        FrameGpuStats over;
        over.sampleExplicit(c, 13, /*tex*/0, /*rt*/71 * MB, 0, 0);
        const auto ro = evaluate(over, TargetTier::Low);
        CHECK(!ro.pass, "71 MB of render targets is OVER the low tier");
        const auto* w = ro.worst();
        CHECK(w && std::string(w->name) == "render targets",
              "worst offender named: %s", w ? w->name : "<none>");
        CHECK(w && w->fraction() > 3.0,
              "and quantified at %.0f%% of its ceiling", w ? w->fraction()*100 : 0);

        // The same frame is comfortable on a desktop — the tier is the policy.
        CHECK(evaluate(over, TargetTier::High).pass,
              "the same frame PASSES the high tier");
    }
    {
        CHECK(parseTier("low") == TargetTier::Low
              && parseTier("HIGH") == TargetTier::High
              && parseTier("medium") == TargetTier::Mid,
              "tier parsing accepts the documented spellings");
        CHECK(parseTier("nonsense") == TargetTier::Low && parseTier(nullptr) == TargetTier::Low,
              "unknown/null tier -> Low (strictest), so a typo cannot silently "
              "disable the budget check");
    }
    {
        // Peak, not final: a frame that briefly spikes still fails.
        FrameGpuStats s;
        HandleCounts c;
        s.sampleExplicit(c, 1, 0, 200 * MB, 0, 0);   // spike
        s.sampleExplicit(c, 1, 0,   1 * MB, 0, 0);   // settles
        CHECK(!evaluate(s, TargetTier::Low).pass,
              "a transient 200 MB spike fails even though the last frame is 1 MB");
    }

    // ── 3. Census / owners / duplicates / leaks ─────────────────────────────
    struct FakeHandle { uint32_t id = 0; };
    using Cache = gpucache::GpuResourceCache<FakeHandle>;
    uint32_t nextId = 1;
    auto factory = [&](size_t bytes) {
        return [&, bytes](FakeHandle& h, size_t& b) {
            h.id = nextId++; b = bytes; return true;
        };
    };
    {
        Cache c;
        FakeHandle h{};
        c.acquire("k_diff", "pistol_diff_4k", factory(11 * MB), h);
        c.acquire("k_nrm",  "pistol_nor_4k",  factory(21 * MB), h);
        c.acquire("k_ui",   "hud/icon.png",   factory(64),      h);

        const auto rep = census(c);
        CHECK(rep.count == 3, "census sees every resident resource (%zu)", rep.count);
        CHECK(rep.rows[0].owner == "pistol_nor_4k",
              "sorted biggest-first: %s", rep.rows[0].owner.c_str());
        uint64_t sum = 0; for (const auto& r : rep.rows) sum += r.bytes;
        CHECK(sum == rep.totalBytes, "every byte accounted (%llu)",
              (unsigned long long)rep.totalBytes);

        const auto owners = byOwner(rep);
        CHECK(owners.size() == 3 && owners[0].owner == "pistol_nor_4k",
              "per-owner profile ranks the 21 MB normal map first");

        CHECK(suspectedDuplicates(rep).empty(),
              "no duplicates — content keying means there cannot be");
    }
    {
        // Two keys, same owner and size: what a dedup regression looks like.
        Cache c;
        FakeHandle h{};
        c.acquire("keyA", "pistol_diff_4k", factory(11 * MB), h);
        c.acquire("keyB", "pistol_diff_4k", factory(11 * MB), h);
        const auto dups = suspectedDuplicates(census(c));
        CHECK(dups.size() == 1 && dups[0].bytes == (uint64_t)(11 * MB),
              "a duplicate IS reported when one exists (%zu found)", dups.size());
    }
    {
        // The leak assertion the soak lane will make.
        Cache c;
        FakeHandle h{};
        const auto base = takeBaseline(c);

        c.acquire("lvl1_wall",  "level1/wall.png",  factory(2 * MB), h);
        c.acquire("lvl1_floor", "level1/floor.png", factory(3 * MB), h);
        c.release("lvl1_wall"); c.release("lvl1_floor");
        c.evictAllUnreferenced();

        const auto clean = compareToBaseline(c, base);
        CHECK(clean.clean(), "matched acquire/release returns to baseline "
              "(%lld bytes)", (long long)clean.bytesDelta);

        c.acquire("hud", "hud/crosshair.png", factory(777), h);   // never released
        c.evictAllUnreferenced();
        const auto leaked = compareToBaseline(c, base);
        CHECK(!leaked.clean(), "an unreleased resource is NOT clean");
        CHECK(leaked.stillReferenced.size() == 1
              && leaked.stillReferenced[0].owner == "hud/crosshair.png"
              && leaked.bytesDelta == 777,
              "leak named and costed: %s, %lld bytes",
              leaked.stillReferenced.empty() ? "?"
                : leaked.stillReferenced[0].owner.c_str(),
              (long long)leaked.bytesDelta);
    }

    if (g_failures) {
        std::printf("render_diag_test: FAIL — %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("render_diag_test: PASS\n");
    return 0;
}
