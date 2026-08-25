// ── stress_physics — 2,000-body rigid-body dump ─────────────────────────────
// Drops 2,000 dynamic boxes into a tight pile on a static floor and steps the
// real JoltPlugin (the engine's physics path, including the 4-step spiral-of-
// death clamp and the engine job-system adapter) for 10 sim-seconds. Catches
// instability (NaN/Inf), tunneling (bodies falling through the floor), and
// explosions; reports the per-step cost under heavy contact load.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include <flecs.h>

#include "core/transform.h"
#include "components/rigid_body.h"
#include "runtime/jobs/jobs.h"
#include "runtime/runtime_context.h"
#include "plugins/jolt_plugin.h"
#include "render/asset_registry.h"
#include "render/texture_registry.h"
#include "render/material_registry.h"
#include "project/project_context.h"
#include "assets/importers/importer_registry.h"

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}
static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

int main() {
    // Unbuffered: ctest redirects stdout, which makes it block-buffered,
    // and a test killed on timeout loses everything still in the buffer.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("stress_physics: 2000-body dump onto a floor\n");
    jobs::init();

    flecs::world w;
    // JoltPlugin::onAttach ignores the context — build a minimal one for it.
    AssetRegistry assets; TextureRegistry tex; MaterialRegistry mat;
    ProjectContext proj; ImporterRegistry imp;
    RuntimeContext ctx{ w, assets, tex, mat, proj, imp };

    // Static floor (top at y=0).
    RigidBody floor; floor.bodyType = PhysicsBodyType::Static;
    floor.halfExtent = {50.0f, 0.5f, 50.0f};
    w.entity().set<Transform>({{0.f,-0.5f,0.f},{0,0,0,1},{1,1,1}}).set<RigidBody>(floor);

    // 2000 dynamic boxes stacked in a column of layers above the floor.
    constexpr int N = 2000;
    std::vector<flecs::entity_t> boxes; boxes.reserve(N);
    for (int i = 0; i < N; ++i) {
        const float x = (i % 10) * 1.2f - 5.4f;
        const float z = ((i / 10) % 10) * 1.2f - 5.4f;
        const float y = 1.0f + (i / 100) * 1.2f;      // 20 layers
        RigidBody rb; rb.bodyType = PhysicsBodyType::Dynamic;
        rb.halfExtent = {0.5f, 0.5f, 0.5f}; rb.mass = 1.0f;
        boxes.push_back(w.entity()
            .set<Transform>({{x,y,z},{0,0,0,1},{1,1,1}}).set<RigidBody>(rb).id());
    }

    JoltPlugin jolt;
    jolt.onAttach(ctx);
    jolt.onSimulationStart(w);

    constexpr int STEPS = 600;                        // 10 s at 60 Hz
    auto t0 = Clock::now();
    for (int s = 0; s < STEPS; ++s) jolt.onPhysicsStep(w, 1.0f / 60.0f);
    auto t1 = Clock::now();

    int nan = 0, escaped = 0; float minY = 1e9f, maxY = -1e9f;
    for (auto id : boxes) {
        const Transform& t = w.entity(id).get<Transform>();
        if (!std::isfinite(t.position.x) || !std::isfinite(t.position.y) ||
            !std::isfinite(t.position.z)) { ++nan; continue; }
        minY = std::min(minY, t.position.y);
        maxY = std::max(maxY, t.position.y);
        if (t.position.y < -5.0f || t.position.y > 200.0f) ++escaped;
    }
    std::printf("        %d bodies x %d steps: %.3f ms/step; pile Y [%.2f, %.2f]\n",
                N, STEPS, ms(t0,t1)/STEPS, minY, maxY);

    CHECK(nan == 0, "no NaN/Inf positions in a 2000-body pile");
    CHECK(escaped == 0, "no body tunneled through the floor or exploded");
    CHECK(minY > -1.0f, "pile rests ON the floor (minY = %.2f)", minY);

    jolt.onSimulationStop();
    jolt.onDetach();
    jobs::shutdown();
    if (g_failures) { std::printf("stress_physics: FAIL — %d\n", g_failures); return 1; }
    std::printf("stress_physics: PASS — 2000-body pile stable\n");
    return 0;
}
