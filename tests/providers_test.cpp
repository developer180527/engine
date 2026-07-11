// ── providers_test — engine providers are PROJECT DATA ──────────────────────
// The swap seam, proven end to end: a project.json "providers" block decides
// which physics/scripting/audio implementation the host constructs
// (stock_plugins.h) — no host-code edit. Discriminator: JoltPlugin publishes
// PhysicsServiceRef into the sim world at onSimulationStart; the null
// provider doesn't. Headless. Exits non-zero on first failure.
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>

#include "runtime/runtime.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/scripting/script_services.h"
#include "plugins/stock_plugins.h"

namespace fs = std::filesystem;
namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

static fs::path makeProject(const char* name, const char* providersJson) {
    const fs::path root = fs::temp_directory_path() / "engine_providers_test" / name;
    fs::remove_all(root);
    fs::create_directories(root / "assets");
    std::ofstream f(root / "project.json");
    f << "{\"name\":\"" << name << "\",\"assetRoot\":\"assets\","
      << "\"providers\":" << providersJson << "}";
    return root;
}

// Boot a headless runtime on `root`, add providers by name, start the sim,
// and report whether a physics service was published.
static bool physicsPublished(const fs::path& root) {
    EngineConfig cfg;
    cfg.projectRoot      = root;
    cfg.defaultScene      = false;
    cfg.openAssetDatabase = false;
    EngineRuntime engine;
    if (!engine.init(cfg, std::make_unique<HeadlessPlatform>())) {
        std::printf("  FAIL  init for %s\n", root.string().c_str());
        ++g_failures;
        return false;
    }
    addStockPlugins(engine);
    engine.attachPlugins();
    engine.startSimulation();
    engine.tickSimulation(1.0f / 60.0f);
    const bool published =
        engine.simWorld().try_get<PhysicsServiceRef>() != nullptr;
    engine.stopSimulation();
    engine.shutdown();
    return published;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("providers_test: project-data provider selection\n");

    // ── 1. Explicit null providers: physics stays unpublished ────────────
    {
        auto root = makeProject("none",
            R"({"physics":"none","scripting":"none","audio":"none"})");
        CHECK(!physicsPublished(root),
              "physics 'none': no PhysicsServiceRef published");
    }

    // ── 2. Defaults (no providers block): jolt publishes ─────────────────
    {
        auto root = makeProject("defaults", R"({})");
        // Empty providers object → all defaults (jolt/lua/miniaudio).
        CHECK(physicsPublished(root),
              "default providers: Jolt publishes PhysicsServiceRef");
    }

    // ── 3. Unknown name falls back LOUDLY to the default ─────────────────
    {
        auto root = makeProject("typo",
            R"({"physics":"pysx","scripting":"none","audio":"none"})");
        CHECK(physicsPublished(root),
              "unknown physics name falls back to jolt (with an error log)");
    }

    fs::remove_all(fs::temp_directory_path() / "engine_providers_test");

    if (g_failures) { std::printf("providers_test: %d FAILURE(S)\n", g_failures); return 1; }
    std::printf("providers_test: ALL PASS\n");
    return 0;
}
