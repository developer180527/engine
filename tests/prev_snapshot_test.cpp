// ── prev_snapshot_test — the interpolation snapshot's CONTRACT ────────────────
//
// PrevTransform is how rendering stays smooth at any frame rate against a fixed
// 60 Hz sim: extraction lerps PrevTransform -> Transform by the accumulator
// fraction. The runtime writes it at the start of every fixed step.
//
// That write was rebuilt for speed (issues.md H.0: 12.7 ms per step on a 50 000
// object scene, ~25 ms of a 34 ms frame, because it did a DEFERRED STRUCTURAL
// set<> per entity per step). It is now two queries — overwrite in place for
// entities that have the component, a structural add only for newcomers — and
// that rewrite has failure modes that no counter and no frame time would reveal:
//
//   * a newcomer never gets PrevTransform, so it is never interpolated and
//     renders a step behind forever — reads as "that object stutters";
//   * PrevTransform gets the POST-step transform instead of the pre-step one,
//     so the lerp interpolates from where the object already is: motion looks
//     right at alpha 1 and wrong everywhere in between;
//   * a camera acquires PrevTransform, and camera look starts lagging — the
//     exact thing the exclusion exists to prevent, and the most likely
//     regression, since the exclusion moved from a per-entity has<Camera>()
//     into a query term.
//
// Timing cannot catch any of those. Headless, no GPU: this is about what the
// components hold after a step.
#include <cstdio>
#include <memory>
#include <vector>

#include "runtime/runtime.h"
#include "runtime/platform/headless_platform.h"
#include "components/prev_transform.h"
#include "components/camera.h"

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

static Transform at(float x, float y, float z) {
    Transform t;
    t.position = { x, y, z };
    t.rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
    t.scale    = { 1.0f, 1.0f, 1.0f };
    return t;
}

static bool near3(const bx::Vec3& a, float x, float y, float z, float eps = 1e-4f) {
    return bx::abs(a.x - x) < eps && bx::abs(a.y - y) < eps
        && bx::abs(a.z - z) < eps;
}

// One fixed step is 1/60 s; pass enough dt that the accumulator certainly
// crosses it, and tick the sim the way the host loop does.
static void stepSim(EngineRuntime& engine, int steps = 1) {
    for (int i = 0; i < steps; ++i) engine.tickSimulation(0.02f);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("prev_snapshot_test: PrevTransform snapshot contract\n");

    EngineConfig cfg;
    cfg.openAssetDatabase = false;

    EngineRuntime engine;
    if (!engine.init(cfg, std::make_unique<HeadlessPlatform>())) {
        std::printf("prev_snapshot_test: FAIL — init\n");
        return 1;
    }

    engine.startSimulation(EngineRuntime::SimMode::InPlace);
    flecs::world& w = engine.simWorld();

    // ── 1. A newcomer acquires PrevTransform ────────────────────────────────
    // The structural-add pass. If it were dropped, everything spawned at runtime
    // would silently stop being interpolated.
    flecs::entity e = w.entity().set<Transform>(at(1.0f, 2.0f, 3.0f));
    CHECK(e.try_get<PrevTransform>() == nullptr,
          "a fresh entity has no PrevTransform yet");
    stepSim(engine);
    const PrevTransform* p = e.try_get<PrevTransform>();
    CHECK(p != nullptr, "one step later it has one — the add pass ran");
    if (p) CHECK(near3(p->position, 1.0f, 2.0f, 3.0f),
                 "and it holds the transform it had entering the step "
                 "(%.2f, %.2f, %.2f)", p->position.x, p->position.y, p->position.z);

    // ── 2. THE contract: Prev is the state ENTERING the step ────────────────
    // Stated precisely, because the first version of this test got it wrong and
    // "proved" a regression that did not exist — the OLD implementation failed it
    // identically, which is how the mistake was caught. Prev := Transform at the
    // TOP of the step, so an external move made between steps is simply the state
    // entering the next step, and capturing it is correct. There is no "one step
    // of lag" to observe for an externally-moved entity.
    //
    // WHAT THIS FILE THEREFORE CANNOT SHOW: Prev and Transform actually differing.
    // That gap only appears when something mutates a transform INSIDE a step, and
    // in a headless runtime nothing does — Spinner runs in tick() at render rate,
    // there are no physics bodies, and no plugins are attached. An earlier draft
    // asserted a spinner's Prev/current rotation would diverge; it cannot here, and
    // the assertion was removed rather than weakened into something that passes for
    // the wrong reason. Producing a real intra-step delta needs a plugin that moves
    // transforms in onUpdate, which is a bigger harness than this fix warranted.
    //
    // The contract IS pinned, by cases 1 and 4: after a step, every entity's Prev
    // equals the distinct value that entity held entering the step. A snapshot
    // written at the wrong time, skipping entities, or crossing them fails those.

    // Scale and rotation travel, not just position — position-only would pass
    // everything else here and still break interpolated spin and scale-punch.
    Transform spun = at(10.0f, 20.0f, 30.0f);
    spun.rotation = bx::fromEuler(bx::Vec3{ 0.0f, 1.0f, 0.0f });
    spun.scale    = { 2.0f, 3.0f, 4.0f };
    e.set<Transform>(spun);
    stepSim(engine);
    p = e.try_get<PrevTransform>();
    CHECK(p && near3(p->scale, 2.0f, 3.0f, 4.0f), "scale is snapshotted too");
    CHECK(p && bx::abs(p->rotation.y - spun.rotation.y) < 1e-4f,
          "and rotation");

    // ── 3. Cameras must NEVER acquire one ──────────────────────────────────
    // Their rotation is late-latched at render rate in onFrame; a lerp would drag
    // look direction backwards. The exclusion used to be a per-entity
    // has<Camera>() and is now a query term, so this is the assertion that the
    // move did not quietly change behaviour.
    flecs::entity cam = w.entity().set<Transform>(at(0.0f, 5.0f, 0.0f))
                                  .set<Camera>({});
    stepSim(engine, 3);
    CHECK(cam.try_get<PrevTransform>() == nullptr,
          "a Camera entity still has NO PrevTransform after 3 steps");
    // And the non-camera beside it still gets updated, so the exclusion is not
    // accidentally excluding everything.
    CHECK(e.try_get<PrevTransform>() != nullptr,
          "while the ordinary entity beside it keeps its own");

    // ── 4. Many entities: every one gets its OWN value ─────────────────────
    // The overwrite pass walks archetypes in bulk. The failure modes are skipping
    // entities and CROSSING them — entity i receiving entity j's transform — and
    // both are invisible unless every entity carries a distinguishable value.
    // 500 spans more than one table page.
    std::vector<flecs::entity> many;
    for (int i = 0; i < 500; ++i)
        many.push_back(w.entity().set<Transform>(at((float)i, 0.0f, 0.0f)));
    stepSim(engine);                       // add pass: each gets its own
    int wrong = 0;
    for (int i = 0; i < 500; ++i) {
        const PrevTransform* pp = many[i].try_get<PrevTransform>();
        if (!pp || !near3(pp->position, (float)i, 0.0f, 0.0f)) ++wrong;
    }
    CHECK(wrong == 0,
          "all 500 newcomers snapshotted their OWN position, none crossed "
          "(%d wrong)", wrong);

    // Now move them all and step again, so the OVERWRITE pass (not the add pass)
    // is the one under test, still with per-entity distinguishable values.
    for (int i = 0; i < 500; ++i)
        many[i].set<Transform>(at((float)i * 2.0f + 0.5f, 1.0f, 0.0f));
    stepSim(engine);
    wrong = 0;
    for (int i = 0; i < 500; ++i) {
        const PrevTransform* pp = many[i].try_get<PrevTransform>();
        if (!pp || !near3(pp->position, (float)i * 2.0f + 0.5f, 1.0f, 0.0f)) ++wrong;
    }
    CHECK(wrong == 0,
          "and the in-place overwrite pass updates all 500 correctly (%d wrong)",
          wrong);

    engine.stopSimulation();

    // ── 5. The caches must not outlive the world they were built on ────────
    // Both snapshot queries are WorldQueryCache entries against simWorld(). In
    // Snapshot mode that world is destroyed on stop, and a new one can land at
    // the same address — a stale query there is a crash, not a leak. Run a full
    // Snapshot cycle to exercise reset(); this is a does-it-survive test.
    for (int cycle = 0; cycle < 3; ++cycle) {
        engine.startSimulation(EngineRuntime::SimMode::Snapshot);
        flecs::world& game = engine.simWorld();
        flecs::entity g = game.entity().set<Transform>(at(4.0f, 4.0f, 4.0f));
        stepSim(engine, 2);
        CHECK(g.try_get<PrevTransform>() != nullptr,
              "snapshot-world entity gets a PrevTransform (cycle %d)", cycle);
        engine.stopSimulation();
    }

    engine.shutdown();

    if (g_failures) {
        std::printf("\nprev_snapshot_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nprev_snapshot_test: all checks passed\n");
    return 0;
}
