// ── provider_debug_draw_test — physics draws without knowing the renderer ─────
// WO-087, provider-abi.md rule 9. Physics debug shapes cross from one provider
// (physics) to another (the renderer). They go through a service the ENGINE
// owns, core/debug_draw.h, never through a renderer's header, so swapping the
// renderer cannot drag physics with it, and the reverse.
//
//   1. Jolt, created by the registry with the engine's ProviderContext, draws
//      every body's bounds into the engine's collector, with the `none` renderer
//      running: no renderer is needed for physics to draw.
//   2. The bgfx renderer turns those same vertices into its own line format
//      (render/pipeline/debug_lines.h): positions and RGBA bytes exactly. A
//      window cannot be had in a test, so this is the GPU-free half of its
//      line pass, the half that reads the collector.
// The physics code is the same in both: it names neither renderer.
#include <cstdio>
#include <memory>
#include <vector>

#include "runtime/runtime.h"
#include "runtime/platform/headless_platform.h"
#include "plugins/stock_plugins.h"
#include "plugins/jolt_plugin.h"
#include "render/pipeline/debug_lines.h"
#include "components/rigid_body.h"
#include "core/transform.h"

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

// One whole frame, as the host loop runs it: frameBegin clears the collector
// (debug lines are per-frame), the tick draws, frameEnd hands it to the renderer.
static void frame(EngineRuntime& e) {
    float dt = 0.0f;
    e.frameBegin(dt);
    e.tick(1.0f / 60.0f);   // a fixed step, not the measured dt: the test is not timing
    e.frameEnd();
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("provider_debug_draw_test: physics debug shapes through the engine's collector\n");

    EngineConfig cfg;
    cfg.openAssetDatabase = false; cfg.autoDetectProject = false; cfg.defaultScene = false;
    EngineRuntime engine;
    CHECK(engine.init(cfg, std::make_unique<HeadlessPlatform>()), "a headless runtime");
    CHECK(engine.rendererName() == "none", "the renderer is `none`");
    CHECK(addProviders(engine), "providers made by the registry, with the engine's context");
    JoltPlugin* jolt = nullptr;
    for (const auto& p : engine.plugins().all())
        if (auto* j = dynamic_cast<JoltPlugin*>(p.get())) jolt = j;
    CHECK(jolt != nullptr, "physics is Jolt");
    if (!jolt) return 1;
    engine.attachPlugins();
    engine.startSimulation(EngineRuntime::SimMode::InPlace);

    flecs::world& w = engine.simWorld();
    RigidBody floorRb{}; floorRb.bodyType = PhysicsBodyType::Static; floorRb.halfExtent = {10, 0.5f, 10};
    w.entity().set<Transform>({{0, -0.5f, 0}, {0, 0, 0, 1}, {1, 1, 1}}).set<RigidBody>(floorRb);
    for (int i = 0; i < 3; ++i) {
        RigidBody rb{}; rb.halfExtent = {0.5f, 0.5f, 0.5f};
        w.entity().set<Transform>({{(float)i * 2.0f, 3.0f, 0}, {0, 0, 0, 1}, {1, 1, 1}}).set<RigidBody>(rb);
    }
    frame(engine);   // the bodies are created

    std::printf("\n-- 1. physics draws into the engine's collector, with no renderer --\n");
    frame(engine);
    CHECK(engine.debugDraw().empty(), "nothing is drawn until the overlay is asked for");
    jolt->setDrawBodies(true);
    frame(engine);
    const std::vector<dbg::DebugVertex> verts = engine.debugDraw().vertices();
    CHECK(verts.size() == 4 * 12 * 2, "four bodies, twelve edges each, two vertices an edge (%zu vertices)", verts.size());
    int green = 0, grey = 0;
    for (const dbg::DebugVertex& v : verts) {
        green += v.color.g == 255 && v.color.r < 128;
        grey  += v.color.r == v.color.g && v.color.g == v.color.b && v.color.r < 200;
    }
    CHECK(green == 3 * 24 && grey == 24, "falling boxes awake (green), the static floor not (grey): %d, %d", green, grey);
    bool inFloor = false;
    for (const dbg::DebugVertex& v : verts)
        inFloor |= v.x == -10.0f && v.y == -1.0f;
    CHECK(inFloor, "the floor's box has its corner where the body is (-10, -1, ...)");
    jolt->setDrawBodies(false);
    frame(engine);
    CHECK(engine.debugDraw().empty(), "turned off, nothing; the collector is cleared each frame");

    std::printf("\n-- 2. the bgfx renderer reads the same vertices into its own format --\n");
    std::vector<render::LineVertex> lines(verts.size());
    render::toLineVertices(verts.data(), (uint32_t)verts.size(), lines.data());
    bool same = true;
    for (std::size_t i = 0; i < verts.size(); ++i) {
        const dbg::DebugVertex& a = verts[i];
        const render::LineVertex& b = lines[i];
        same &= a.x == b.x && a.y == b.y && a.z == b.z && a.color.r == b.rgba[0] && a.color.g == b.rgba[1]
             && a.color.b == b.rgba[2] && a.color.a == b.rgba[3];
    }
    CHECK(same, "every position and RGBA byte, in its line layout's order");
    const dbg::Color c = dbg::packRGBA(1.0f, 0.5f, 0.0f, 0.25f);
    CHECK(c.r == 255 && c.g == 128 && c.b == 0 && c.a == 64, "the engine's colour is R, G, B, A");
    const dbg::Color clamped = dbg::packRGBA(2.0f, -1.0f, NAN);
    CHECK(clamped.r == 255 && clamped.g == 0 && clamped.b == 0 && clamped.a == 255, "out of range clamps; NaN is 0");

    engine.stopSimulation();
    engine.shutdown();
    if (g_failures) { std::printf("\nprovider_debug_draw_test: %d FAILURE(S)\n", g_failures); return 1; }
    std::printf("\nprovider_debug_draw_test: all checks passed\n");
    return 0;
}
