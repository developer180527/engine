// ── empty_game_budget_test — an empty game costs (almost) nothing (WO-050) ────
//
// Owner rule (2026-10-01): developers' scripts will be expensive, so the engine
// must be nearly free when there is nothing to do. With no assets and no
// scripts, only the main loop running, its own memory and per-frame cost must
// be negligible. A minimal 2D game must not carry the 3D engine.
//
// This is the BUDGET that turns red when something is sized for a big game
// up front again. It runs the real stock plugins (physics, scripting, audio:
// what engine_host and engine_player get) on an empty simulation, headless,
// and checks every tagged heap and the time per tick. Measured before WO-050:
// physics alone held 78 MB live (Jolt sized for 65 536 bodies at start).
//
// Render targets need a GPU and are measured windowed instead (WO-050's log).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

#include "core/memory/mem.h"
#include "plugins/stock_plugins.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/runtime.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

// The budget. Generous enough to be stable across machines, tight enough that
// a reservation for big content (megabytes) cannot hide in it.
constexpr double kTaggedBudgetMB = 8.0;     // all tagged heaps together, live
constexpr double kTickBudgetMs   = 1.0;     // median wall time of one empty tick

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("empty_game_budget_test\n");

    EngineConfig cfg;
    cfg.openAssetDatabase = false; cfg.autoDetectProject = false; cfg.defaultScene = false;
    // An EMPTY project root, so nothing in the working directory (a repo's
    // scripts/autorun, say) is picked up as content.
    cfg.projectRoot = std::filesystem::temp_directory_path() / "engine_empty_game_budget";
    std::filesystem::remove_all(cfg.projectRoot);
    std::filesystem::create_directories(cfg.projectRoot);
    EngineRuntime engine;
    CHECK(engine.init(cfg, std::make_unique<HeadlessPlatform>()), "a headless runtime");
    addStockPlugins(engine);
    engine.attachPlugins();
    engine.startSimulation(EngineRuntime::SimMode::InPlace);
    for (int t = 0; t < 60; ++t) engine.tick(1.0f / 60.0f);   // warm up

    std::vector<double> ms;
    for (int t = 0; t < 300; ++t) {
        const auto t0 = std::chrono::steady_clock::now();
        engine.tick(1.0f / 60.0f);
        ms.push_back(std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(ms.begin(), ms.end());
    const double median = ms[ms.size() / 2];

    struct Row { const char* name; mem::Tag tag; };
    const Row rows[] = {
        {"Core", mem::Tag::Core}, {"Rendering", mem::Tag::Rendering},
        {"Physics", mem::Tag::Physics}, {"Scripting", mem::Tag::Scripting},
        {"ECS", mem::Tag::ECS}, {"Audio", mem::Tag::Audio}, {"Jobs", mem::Tag::Jobs},
        {"Animation", mem::Tag::Animation}, {"Assets", mem::Tag::Assets},
        {"Frame", mem::Tag::Frame}, {"Nav", mem::Tag::Nav}, {"Sim", mem::Tag::Sim},
        {"Replay", mem::Tag::Replay},
    };
    double total = 0;
    for (const Row& r : rows) {
        const double mb = mem::stats(r.tag).currentBytes / 1048576.0;
        total += mb;
        std::printf("        %-10s %7.3f MB live\n", r.name, mb);
    }
    CHECK(mem::stats(mem::Tag::Physics).currentBytes < 64 * 1024,
          "physics holds nothing with nothing to simulate (%llu bytes; was 78 MB)",
          (unsigned long long)mem::stats(mem::Tag::Physics).currentBytes);
    CHECK(total < kTaggedBudgetMB, "every tagged heap together: %.2f MB live (budget %.0f MB)",
          total, kTaggedBudgetMB);
    CHECK(median < kTickBudgetMs, "one empty tick: %.3f ms median (budget %.1f ms)",
          median, kTickBudgetMs);

    engine.stopSimulation();
    engine.shutdown();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
