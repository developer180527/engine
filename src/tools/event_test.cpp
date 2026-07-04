// ── event_test — EventSweeper lifecycle gauntlet ────────────────────────────
// Proves the guarantee the event model exists for: a message written at ANY
// point in a tick is observable for at least one FULL subsequent tick before it
// expires, no matter who reads it first — and that draining (consume) or
// re-firing behaves without orphaning the staleness marker. Standalone: a bare
// flecs world + the sweeper, no engine boot. Exits non-zero on first failure.
#include <cstdio>

#include <flecs.h>

#include "components/event_component.h"
#include "runtime/event_sweeper.h"

namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d)\n", __FILE__, __LINE__); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

// A stand-in event payload (mirrors combat::DamageInbox's role).
struct Ping { int n = 0; };

int main() {
    std::printf("event_test: EventSweeper lifecycle gauntlet\n");

    flecs::world w;
    events::declare<Ping>(w);
    CHECK(w.component<Ping>().has<EventComponent>(), "declare tags EventComponent");
    CHECK(w.component<Ping>().has<SerdeTransient>(), "declare implies SerdeTransient");

    EventSweeper sweep;

    // ── Lifetime: an unconsumed event lives exactly through the next tick ────
    // Written "mid tick N"; then we run the start-of-tick sweep for N+1, N+2.
    {
        flecs::entity e = w.entity().set<Ping>({1});
        CHECK(e.has<Ping>(), "event present the tick it was written (N)");

        sweep.sweep(w);   // start of N+1: mark stale, keep
        CHECK(e.has<Ping>(), "event still present for the WHOLE next tick (N+1)");

        sweep.sweep(w);   // start of N+2: stale -> expire
        CHECK(!e.has<Ping>(), "unconsumed event expires after its full tick (N+2)");
        e.destruct();
    }

    // ── Drain short-circuits: consume() ends the event immediately, cleanly ──
    // Consume AFTER the stale mark (the load-order-inverted case) then re-fire:
    // the marker must not orphan and prematurely kill the re-fired event.
    {
        flecs::entity e = w.entity().set<Ping>({2});
        sweep.sweep(w);                          // N+1: marked stale
        CHECK(e.has<Ping>(), "delivered on N+1 (consumer's tick)");
        events::consume<Ping>(e);                // consumer drains it
        CHECK(!e.has<Ping>(), "consume() removes the event now");

        // Re-fire on the SAME entity — this is the orphan-marker trap.
        e.set<Ping>({3});
        sweep.sweep(w);                          // must MARK (fresh), not sweep
        CHECK(e.has<Ping>(), "re-fired event gets its own full tick, not expired early");
        sweep.sweep(w);                          // now it may expire
        CHECK(!e.has<Ping>(), "re-fired event then expires on schedule");
        e.destruct();
    }

    // ── Many carriers, mixed ages, one sweep pass ───────────────────────────
    {
        flecs::entity a = w.entity().set<Ping>({0});
        sweep.sweep(w);                          // a marked stale
        flecs::entity b = w.entity().set<Ping>({0});   // b fresh, same tick
        sweep.sweep(w);                          // a expires, b marked
        CHECK(!a.has<Ping>() && b.has<Ping>(),
              "independent ages: older expires while younger survives");
        b.destruct();
    }

    // ── Sweeper survives a world with the registry but zero live events ──────
    { sweep.sweep(w); CHECK(true, "empty sweep is a no-op, no crash"); }

    sweep.reset();   // release queries before the world unwinds
    if (g_failures) { std::printf("event_test: FAIL — %d failure(s)\n", g_failures);
                      return 1; }
    std::printf("event_test: PASS — event lifecycle guaranteed\n");
    return 0;
}
