// ── stress_churn — spawn/destroy entity churn ───────────────────────────────
// Rapidly creates and destroys entities (with varying component sets → flecs
// table churn) plus one-shot EVENT components (event sweeper under load), for
// thousands of ticks. Catches leaks / unbounded fragmentation (memory must not
// grow with total work done, only with the live set) and confirms flecs +
// the sweeper survive heavy structural churn without crashing.
#include <cstdio>
#include <vector>

#include <flecs.h>

#include "core/transform.h"
#include "core/memory/mem.h"
#include "components/event_component.h"
#include "runtime/event_sweeper.h"

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

struct Ping { int n = 0; };            // event payload
struct Tag2 {};
struct Blob { double a, b, c, d; };

int main() {
    std::printf("stress_churn: spawn/destroy + event-sweeper churn\n");

    flecs::world w;
    events::declare<Ping>(w);
    EventSweeper sweeper;

    constexpr int TICKS   = 8000;
    constexpr int PERTICK = 400;    // create+destroy this many per tick
    std::vector<flecs::entity_t> live;
    live.reserve(PERTICK * 4);

    // Warm the allocator/tables to the churn's working set, then snapshot.
    for (int i = 0; i < PERTICK * 4; ++i)
        live.push_back(w.entity().set<Transform>({}).id());
    for (auto id : live) w.entity(id).destruct();
    live.clear();
    const uint64_t maps0 = mem::mapEventCount();

    uint32_t rng = 0x1234567u;
    for (int t = 0; t < TICKS; ++t) {
        sweeper.sweep(w);                          // age events at tick start
        // Create a batch with mixed component sets (drives table variety).
        for (int i = 0; i < PERTICK; ++i) {
            rng = rng*1664525u + 1013904223u;
            flecs::entity e = w.entity().set<Transform>({});
            if (rng & 1)   e.add<Tag2>();
            if (rng & 2)   e.set<Blob>({(double)i,0,0,0});
            if (rng & 4)   e.set<Ping>({i});        // one-shot event
            live.push_back(e.id());
        }
        // Destroy the OLDEST batch once we're two batches deep (rolling window).
        if ((int)live.size() > PERTICK * 2) {
            for (int i = 0; i < PERTICK; ++i) {
                w.entity(live.front()).destruct();
                live.erase(live.begin());
            }
        }
    }
    // Drain the rest.
    for (auto id : live) if (w.entity(id).is_alive()) w.entity(id).destruct();
    live.clear();
    for (int i = 0; i < 4; ++i) sweeper.sweep(w);   // let any events expire
    sweeper.reset();

    const uint64_t newMaps = mem::mapEventCount() - maps0;
    std::printf("        %d ticks x %d create+destroy = %d structural ops; %llu new OS maps\n",
                TICKS, PERTICK*2, TICKS*PERTICK*2, (unsigned long long)newMaps);

    // The whole point: churning millions of entities must NOT leak mappings.
    // A leak would grow linearly with TICKS; caching is bounded + small.
    CHECK(newMaps < 8192,
          "no memory leak/runaway under %d structural ops (%llu maps)",
          TICKS*PERTICK*2, (unsigned long long)newMaps);
    CHECK(w.count<Transform>() == 0, "all churned entities reclaimed (0 live)");

    if (g_failures) { std::printf("stress_churn: FAIL — %d\n", g_failures); return 1; }
    std::printf("stress_churn: PASS — heavy churn, no leak, no crash\n");
    return 0;
}
