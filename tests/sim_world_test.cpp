// ── sim_world_test — Snapshot play must tick the RENDERED world (audit H.2) ─
// tickSystems built its spinner query once on m_ecs (the edit world). In
// Snapshot mode the world actually simulated and rendered is the game-world
// copy — a Spinner living THERE (spawned by gameplay/scripts at runtime; the
// scene snapshot deliberately excludes procedural spinners) never rotated,
// while the hidden edit world kept animating for nobody. The query now
// follows simWorld() via WorldQueryCache. Headless. Exits non-zero on first
// failure.
#include <cstdio>
#include <memory>

#include "runtime/runtime.h"
#include "runtime/platform/headless_platform.h"

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

static flecs::entity spawnSpinner(flecs::world& w) {
    Transform t{};
    t.position = {0, 0, 0};
    t.rotation = {0, 0, 0, 1};
    t.scale    = {1, 1, 1};
    return w.entity().set<Transform>(t).set<Spinner>({1.0f, 0.5f});
}
static bool rotated(flecs::entity e) {
    const bx::Quaternion q = e.get<Transform>().rotation;
    return bx::abs(q.x) + bx::abs(q.y) + bx::abs(q.z) > 1e-4f
        || bx::abs(q.w - 1.0f) > 1e-4f;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("sim_world_test: snapshot-play world routing gauntlet\n");

    EngineConfig cfg;
    cfg.openAssetDatabase = false;

    EngineRuntime engine;
    if (!engine.init(cfg, std::make_unique<HeadlessPlatform>())) {
        std::printf("sim_world_test: FAIL — init\n");
        return 1;
    }

    const float dt = 0.016f;

    // ── 1. Editing (no sim): spinners spin in the edit world ─────────────
    {
        flecs::entity s = spawnSpinner(engine.simWorld());   // == edit world
        for (int i = 0; i < 30; ++i) engine.tick(dt);
        CHECK(rotated(s), "edit-world spinner spins while editing");
        s.destruct();
    }

    // ── 2. Snapshot play: a spinner IN the rendered game world spins ─────
    // This is the H.2 repro: pre-fix, the query was bound to m_ecs, so a
    // runtime-spawned game-world spinner stayed frozen forever.
    {
        engine.startSimulation(EngineRuntime::SimMode::Snapshot);
        flecs::world& game = engine.simWorld();
        flecs::entity s = spawnSpinner(game);
        for (int i = 0; i < 30; ++i) { engine.tick(dt); engine.tickSimulation(dt); }
        CHECK(rotated(s),
              "SNAPSHOT-play spinner rotates in the rendered world — the H.2 freeze");
        engine.stopSimulation();
    }

    // ── 3. Play/Stop/Play: cache rebinds to the fresh game world ─────────
    {
        engine.startSimulation(EngineRuntime::SimMode::Snapshot);
        flecs::entity s = spawnSpinner(engine.simWorld());
        for (int i = 0; i < 30; ++i) { engine.tick(dt); engine.tickSimulation(dt); }
        CHECK(rotated(s), "second session spins too (cache rebound after stop)");
        engine.stopSimulation();
    }

    engine.shutdown();

    if (g_failures) { std::printf("sim_world_test: %d FAILURE(S)\n", g_failures); return 1; }
    std::printf("sim_world_test: ALL PASS\n");
    return 0;
}
