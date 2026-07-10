// ── script_host_test — ScriptHost world-bind lifecycle gauntlet ─────────────
// Regression for the C.1 audit find: setWorld() registered flecs observers
// (capturing `this`) without ever storing/destroying them. Two failure modes:
//   1. InPlace mode re-binds the same immortal world every Play/Stop —
//      2 leaked observers accumulated per cycle.
//   2. ~EngineRuntime destroys the ScriptHost BEFORE m_ecs (reverse member
//      order) — m_ecs tearing down Name entities fired OnRemove into a freed
//      host: use-after-free on ordinary shutdown.
// Run under the ASan lane (build-asan) to prove scenario 2 stays dead.
// Exits non-zero on first failure.
#include <cstdio>
#include <memory>

#include <flecs.h>

#include "runtime/scripting/script_host.h"

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

// Count live observer entities in a world (flecs tags them EcsObserver).
static int observerCount(flecs::world& w) {
    int n = 0;
    w.query_builder().with(flecs::Observer).build()
        .each([&](flecs::entity) { ++n; });
    return n;
}

int main() {
    std::printf("script_host_test: world-bind lifecycle gauntlet\n");

    // ── 1. Observer accumulation across Play/Stop cycles (InPlace mode) ──
    {
        flecs::world w;
        ScriptHost host(&w);
        host.setWorld(&w);                    // first Play
        const int baseline = observerCount(w);
        for (int i = 0; i < 10; ++i) {        // 10 Play/Stop cycles
            host.setWorld(nullptr);
            host.setWorld(&w);
        }
        CHECK(observerCount(w) == baseline,
              "10 Play/Stop cycles: observer count flat (%d == %d baseline)",
              observerCount(w), baseline);

        host.setWorld(nullptr);               // final Stop
        CHECK(observerCount(w) == baseline - 2,
              "unbind removes both Name observers");
    }

    // ── 2. Index correctness through the owned observers ─────────────────
    {
        flecs::world w;
        ScriptHost host(&w);
        host.setWorld(&w);

        flecs::entity e = host.create("zombie_7");
        CHECK(host.find("zombie_7") == e, "find() resolves via observer index");

        e.destruct();
        CHECK(!host.find("zombie_7"),
              "OnRemove observer heals the index on destruct");

        // Pre-existing names must be primed on a re-bind.
        flecs::entity pre = w.entity().set<Name>({"preexisting"});
        host.setWorld(nullptr);
        host.setWorld(&w);
        CHECK(host.find("preexisting") == pre, "re-bind primes existing names");
    }

    // ── 3. The shutdown UAF: host dies while still bound, world outlives ──
    // Mirrors ~EngineRuntime's member order (ScriptHost freed before m_ecs).
    // Pre-fix: world teardown fires OnRemove into the freed host. Post-fix:
    // the unbind-at-destruction token makes surviving lambdas inert.
    {
        auto w = std::make_unique<flecs::world>();
        {
            auto host = std::make_unique<ScriptHost>(w.get());
            host->setWorld(w.get());
            for (int i = 0; i < 32; ++i) {
                char name[32];
                std::snprintf(name, sizeof name, "doomed_%d", i);
                host->create(name);
            }
        }   // host freed HERE — never unbound (the pathological order)

        w.reset();  // world teardown fires every OnRemove — must be inert
        CHECK(true, "world outliving a still-bound host tears down clean");
    }

    if (g_failures) { std::printf("script_host_test: %d FAILURE(S)\n", g_failures); return 1; }
    std::printf("script_host_test: ALL PASS\n");
    return 0;
}
