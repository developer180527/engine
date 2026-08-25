// ── stress_churn — spawn/destroy entity churn ───────────────────────────────
// Rapidly creates and destroys entities (with varying component sets → flecs
// table churn) plus one-shot EVENT components (event sweeper under load), for
// thousands of ticks. Catches leaks / unbounded fragmentation (memory must not
// grow with total work done, only with the live set) and confirms flecs +
// the sweeper survive heavy structural churn without crashing.
#include <cstdio>
#include <cstdlib>
#include <string>
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

int main(int argc, char** argv) {
    // Unbuffered: ctest redirects stdout, which makes it block-buffered,
    // and a test killed on timeout loses everything still in the buffer.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("stress_churn: spawn/destroy + event-sweeper churn\n");

    flecs::world w;
    events::declare<Ping>(w);
    EventSweeper sweeper;

    // --soak [N]: marathon mode — run N ticks (default 200k) and sample memory
    // across the run to catch a SLOW leak (a trend that a short run misses).
    // Pass a big N to soak for hours; the trend print makes a leak obvious.
    int  TICKS = 8000;
    bool soak  = false;
    for (int a = 1; a < argc; ++a) {
        if (std::string(argv[a]) == "--soak") {
            soak = true; TICKS = 200000;
            if (a + 1 < argc && argv[a+1][0] != '-') TICKS = std::atoi(argv[a+1]);
        }
    }
    constexpr int PERTICK = 400;    // create+destroy this many per tick
    std::vector<flecs::entity_t> live;
    live.reserve(PERTICK * 4);

    // Warm the allocator/tables to the churn's working set, then snapshot.
    for (int i = 0; i < PERTICK * 4; ++i)
        live.push_back(w.entity().set<Transform>({}).id());
    for (auto id : live) w.entity(id).destruct();
    live.clear();
    const uint64_t maps0 = mem::mapEventCount();
    uint64_t soakMid = 0;   // map count at the run's midpoint (past warmup drift)

    uint32_t rng = 0x1234567u;
    for (int t = 0; t < TICKS; ++t) {
        // Soak sampling: print the memory trend so a slow leak is visible.
        if (soak && t % (TICKS / 10) == 0) {
            const uint64_t m = mem::mapEventCount();
            std::printf("        [soak %3d%%] live=%d  OS-maps=+%llu\n",
                        t * 100 / TICKS, (int)w.count<Transform>(),
                        (unsigned long long)(m - maps0));
            if (t == TICKS / 2) soakMid = m;
        }
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

    if (soak) {   // the strong leak test: steady state adds ~0 over the 2nd half
        const uint64_t secondHalf = mem::mapEventCount() - soakMid;
        std::printf("        soak: 2nd-half OS maps +%llu (flat = no slow leak)\n",
                    (unsigned long long)secondHalf);
        CHECK(secondHalf < 512, "SOAK: memory flat over the run's second half");
    }

    if (g_failures) { std::printf("stress_churn: FAIL — %d\n", g_failures); return 1; }
    std::printf("stress_churn: PASS — heavy churn, no leak, no crash\n");
    return 0;
}
