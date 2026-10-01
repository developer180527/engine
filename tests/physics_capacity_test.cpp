// ── physics_capacity_test — the physics world grows with content (WO-050) ────
//
// An empty game must not pay for physics it does not have. The world used to be
// created at simulation start with capacity for 65 536 bodies and a 64 MB temp
// allocator: 78 MB live in an empty project. Now nothing is allocated until the
// first body, and the world is REBUILT larger when the content outgrows it,
// because Jolt cannot resize a PhysicsSystem.
//
//   1. an empty simulation allocates no physics memory at all
//   2. a body spawned mid-play creates the world, sized to the content
//   3. outgrowing the plan rebuilds it, bodies keep their place and velocity,
//      and the boxes resting on the floor through the rebuild see NO collision
//      event: a rebuilt world re-reports every contact as new, and those
//      re-reports must not reach scripts as enters (nor the missing ones as exits)
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <vector>

#include <flecs.h>

#include "components/collision_events.h"
#include "components/rigid_body.h"
#include "core/memory/mem.h"
#include "core/transform.h"
#include "plugins/jolt_plugin.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/runtime.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

static flecs::entity box(flecs::world& w, float x, float y, float z, bool dynamic = true) {
    RigidBody rb{};
    rb.bodyType   = dynamic ? PhysicsBodyType::Dynamic : PhysicsBodyType::Static;
    rb.halfExtent = {0.5f, 0.5f, 0.5f};
    rb.mass       = 1;
    return w.entity().set<Transform>({{x, y, z}, {0, 0, 0, 1}, {1, 1, 1}}).set<RigidBody>(rb);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("physics_capacity_test\n");

    EngineConfig cfg;
    cfg.openAssetDatabase = false; cfg.autoDetectProject = false; cfg.defaultScene = false;
    EngineRuntime engine;
    CHECK(engine.init(cfg, std::make_unique<HeadlessPlatform>()), "a headless runtime");
    auto jolt = std::make_shared<JoltPlugin>();
    engine.plugins().add(jolt);
    engine.attachPlugins();
    flecs::world& w = engine.simWorld();

    // ── 1. Empty: nothing allocated ─────────────────────────────────────────
    std::printf("1. an empty simulation\n");
    const size_t before = mem::stats(mem::Tag::Physics).currentBytes;
    engine.startSimulation(EngineRuntime::SimMode::InPlace);
    for (int t = 0; t < 30; ++t) engine.tick(1.0f / 60.0f);
    const size_t emptyLive = mem::stats(mem::Tag::Physics).currentBytes - before;
    CHECK(jolt->simulationActive() && jolt->capacity().bodies == 0,
          "simulating, with no physics world created");
    CHECK(emptyLive < 64 * 1024, "physics memory for an empty game: %zu bytes (was 78 MB)", emptyLive);

    // ── 2. A body arrives mid-play: the world is created for it ────────────
    std::printf("2. content arrives\n");
    RigidBody floorRb{}; floorRb.bodyType = PhysicsBodyType::Static; floorRb.halfExtent = {50, 0.5f, 50};
    const flecs::entity floor = w.entity().set<Transform>({{0, -0.5f, 0}, {0, 0, 0, 1}, {1, 1, 1}})
                                          .set<RigidBody>(floorRb);
    std::vector<flecs::entity> resting;
    for (int i = 0; i < 10; ++i) resting.push_back(box(w, (float)i * 2.0f - 9.0f, 0.5f, 0));
    engine.tick(1.0f / 60.0f);
    const JoltPlugin::Capacity c1 = jolt->capacity();
    CHECK(c1.bodies == JoltPlugin::kMinBodies && c1.constraints > 0,
          "created on the first body, at the smallest tier (%u bodies, %u pairs, %u constraints)",
          c1.bodies, c1.pairs, c1.constraints);
    const size_t smallLive = mem::stats(mem::Tag::Physics).currentBytes - before;
    std::printf("        physics memory with 11 bodies: %.2f MB\n", smallLive / 1048576.0);

    // Let the boxes settle on the floor, then note how the box/floor contacts stand.
    for (int t = 0; t < 120; ++t) engine.tick(1.0f / 60.0f);
    auto restingEvents = [&] {
        int n = 0;
        for (const flecs::entity& b : resting)
            if (const CollisionEvents* ce = b.try_get<CollisionEvents>()) {
                for (flecs::entity_t o : ce->entered) n += o == floor.id();
                for (flecs::entity_t o : ce->exited)  n += o == floor.id();
            }
        return n;
    };
    int touching = 0;
    for (const flecs::entity& b : resting) touching += b.has<CollisionEvents>();
    CHECK(touching == 10, "all ten boxes have touched the floor (%d)", touching);
    float y0[10];
    for (int i = 0; i < 10; ++i) y0[i] = resting[i].get<Transform>().position.y;

    // ── 3. Outgrow the plan: the world is rebuilt, contacts carry across ───
    std::printf("3. outgrow it\n");
    const uint32_t planned = JoltPlugin::plannedMovableFor(c1);
    for (uint32_t i = 0; i < planned + 8; ++i)   // far away, over their own spot
        box(w, 200.0f + (float)(i % 20) * 2.0f, 5.0f + (float)(i / 20) * 2.0f, 200.0f);
    int spurious = 0;
    for (int t = 0; t < 60; ++t) {
        engine.tick(1.0f / 60.0f);
        const int n = restingEvents();
        if (n && std::getenv("CAP_DEBUG")) {
            int in = 0, out = 0;
            for (const flecs::entity& b : resting)
                if (const CollisionEvents* ce = b.try_get<CollisionEvents>()) {
                    in += (int)ce->entered.size(); out += (int)ce->exited.size();
                }
            std::printf("        tick %d: %d enter(s), %d exit(s)\n", t, in, out);
        }
        spurious += n;
    }
    const JoltPlugin::Capacity c2 = jolt->capacity();
    CHECK(jolt->rebuildCount() >= 1 && c2.constraints > c1.constraints,
          "rebuilt larger (%u rebuild(s); constraints %u -> %u)", jolt->rebuildCount(),
          c1.constraints, c2.constraints);
    CHECK(spurious == 0, "the resting boxes saw no enter or exit across the rebuild (%d)", spurious);
    float drift = 0;
    for (int i = 0; i < 10; ++i) drift = std::max(drift, std::fabs(resting[i].get<Transform>().position.y - y0[i]));
    CHECK(drift < 0.01f, "and stayed where they were (max drift %.4f m)", drift);

    // ── 4. The same, for boxes touching and AWAKE at the rebuild ───────────
    // Sleeping bodies have no contacts (Jolt reports them ended at sleep), so
    // §3 never exercises the re-report suppression. These land and the world
    // is grown a few ticks later, well before anything can sleep (0.5 s).
    std::printf("4. a rebuild while they are awake and touching\n");
    std::vector<flecs::entity> awake;
    for (int i = 0; i < 10; ++i) awake.push_back(box(w, (float)i * 2.0f - 9.0f, 0.5f, -20.0f));
    int landed = 0;
    for (int t = 0; t < 5; ++t) {
        engine.tick(1.0f / 60.0f);
        for (const flecs::entity& b : awake)
            if (const CollisionEvents* ce = b.try_get<CollisionEvents>())
                for (flecs::entity_t o : ce->entered) landed += o == floor.id();
    }
    CHECK(landed == 10, "ten boxes landed, each entered the floor once (%d)", landed);
    const uint32_t rebuildsBefore = jolt->rebuildCount();
    const uint32_t planned2 = JoltPlugin::plannedMovableFor(jolt->capacity());
    for (uint32_t i = 0; i < planned2; ++i)
        box(w, -200.0f - (float)(i % 20) * 2.0f, 5.0f + (float)(i / 20) * 2.0f, -200.0f);
    int awakeEvents = 0;
    for (int t = 0; t < 15; ++t) {
        engine.tick(1.0f / 60.0f);
        for (const flecs::entity& b : awake)
            if (const CollisionEvents* ce = b.try_get<CollisionEvents>()) {
                for (flecs::entity_t o : ce->entered) awakeEvents += o == floor.id();
                for (flecs::entity_t o : ce->exited)  awakeEvents += o == floor.id();
            }
    }
    CHECK(jolt->rebuildCount() > rebuildsBefore, "the world was rebuilt again (%u)", jolt->rebuildCount());
    CHECK(awakeEvents == 0,
          "awake, touching boxes saw no re-reported enter and no false exit across it (%d)", awakeEvents);

    // A real contact after the rebuild is still reported: drop a new box.
    const flecs::entity dropped = box(w, 0.0f, 3.0f, 10.0f);
    int liftEnters = 0;
    for (int t = 0; t < 120; ++t) {
        engine.tick(1.0f / 60.0f);
        if (const CollisionEvents* ce = dropped.try_get<CollisionEvents>())
            for (flecs::entity_t o : ce->entered) liftEnters += o == floor.id();
    }
    CHECK(liftEnters >= 1, "a genuine landing after the rebuild is still an enter (%d)", liftEnters);

    engine.stopSimulation();
    CHECK(jolt->capacity().bodies == 0, "stopping releases the world");
    engine.shutdown();
    std::printf("%s (%d failure%s)\n", g_failures ? "FAILED" : "PASSED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
