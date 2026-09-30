// ── sim_clock_test — a fixed step reads no clock (WO-043, audit DET-01) ───────
//
// Each fixed step folds the input staged up to its boundary into its
// InputSnapshot. The boundary was hid::nowNs(), read INSIDE the step, so which
// events a tick saw depended on when the CPU happened to run it. The worst of
// it was the catch-up frame: the accumulator runs several steps back to back,
// the first step's "now" swallowed every event the frame had staged, and the
// rest saw none.
//
// Now the frame's one clock reading is the time input was pumped, and each
// step's boundary is derived from it (src/runtime/sim_clock.h).
//   1. the boundary function: spacing, the leftover, never backwards, edges
//   2. the runtime: one catch-up frame of three steps; the left button pressed,
//      released and pressed again, one change inside each step's slice of real
//      time. Each tick sees exactly its own. The old code folded all three into
//      the first tick.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>

#include <flecs.h>
#include <hid/hid.h>

#include "components/entity_id.h"
#include "components/name.h"
#include "core/transform.h"
#include "runtime/input/input_manager.h"
#include "runtime/input/input_sources.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/runtime.h"
#include "runtime/sim_clock.h"
#include "runtime/sim_intent.h"

using namespace input;

static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

static constexpr float    kSimDt   = 1.0f / 60.0f;
static constexpr uint64_t kSimDtNs = 16666667;
static constexpr uint64_t kPlayer  = 0xC0FFEE01ull;

static const char* kConfig = R"({
  "contexts": [ { "name": "Gameplay", "actions": [
      { "name": "Fire", "type": "digital", "bindings": ["mouse:left"] } ] } ]
})";

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("sim_clock_test — the fixed step's input windows come from the frame\n");

    // ── 1. The boundary function ────────────────────────────────────────────
    std::printf("\n-- 1. simclock::inputTickEndNs --\n");
    {
        using simclock::inputTickEndNs;
        const uint64_t pump = 10'000'000'000ull;   // 10 s
        // Three steps of one frame: the accumulator after each decrement.
        const double leftovers[] = {2.0 * kSimDt + 0.001, 1.0 * kSimDt + 0.001, 0.001};
        uint64_t prev = 0, ends[3];
        for (int i = 0; i < 3; ++i) prev = ends[i] = inputTickEndNs(pump, leftovers[i], prev);
        CHECK(ends[2] == pump - 1'000'000, "the last step ends the leftover (1 ms) before the pump");
        CHECK(std::llabs((long long)(ends[1] - ends[0]) - (long long)kSimDtNs) < 1000 &&
              std::llabs((long long)(ends[2] - ends[1]) - (long long)kSimDtNs) < 1000,
              "successive steps end kSimDt apart (%llu, %llu ns)",
              (unsigned long long)(ends[1] - ends[0]), (unsigned long long)(ends[2] - ends[1]));
        CHECK(inputTickEndNs(pump, 0.050, pump) == pump,
              "a window never ends before the previous one (a frame dt on another clock can pull it back)");
        CHECK(inputTickEndNs(1000, 1.0, 0) == 0, "a leftover larger than the clock itself clamps to 0, not a wrap");
        CHECK(inputTickEndNs(0, 0.0, 0) == 0, "before the first pump every window ends at 0");
        CHECK(inputTickEndNs(pump, -0.5, 0) == pump, "a negative leftover (float residue) counts as none");
    }

    // ── 2. The runtime: a catch-up frame shares its input out ───────────────
    std::printf("\n-- 2. a catch-up frame --\n");
    {
        EngineConfig cfg;
        cfg.openAssetDatabase = false;
        cfg.autoDetectProject = false;
        cfg.defaultScene      = false;
        cfg.projectRoot       = std::filesystem::temp_directory_path() / "engine_sim_clock_root";
        std::filesystem::create_directories(cfg.projectRoot);
        EngineRuntime engine;
        CHECK(engine.init(cfg, std::make_unique<HeadlessPlatform>()), "a headless runtime");

        auto src  = std::make_unique<ReplaySource>();
        auto* raw = src.get();
        raw->addDevice({1, hid::DeviceClass::Mouse, 0x1234, 0x5678, 42, "mouse"});
        engine.inputManager().initWithSource(std::move(src));
        engine.inputManager().loadConfigText(kConfig);
        engine.attachPlugins();
        engine.simWorld().entity().set<Transform>({{0,0,0},{0,0,0,1},{1,1,1}})
                                  .set<Name>({"player"}).set<EntityId>({kPlayer});
        engine.actionSet().declare("Fire");
        engine.setLocalController(kPlayer);
        engine.setCommandRecording(true, 64);
        engine.startSimulation(EngineRuntime::SimMode::InPlace);

        // One frame worth three steps, plus 1 ms left over. The pump happens a
        // few microseconds after t0, so the steps' windows end near
        // t0 - 34.3 ms, t0 - 17.7 ms and t0 - 1 ms. The left button is pressed
        // in the first window, released in the second and pressed again in the
        // third, each well inside its window. Button state reaches the intent
        // through the tick's snapshot, which is exactly what the boundary
        // decides. (The intent's LOOK delta does not: it diffs a total that
        // grows at pump time, by design, so a frame's motion lands in its first
        // tick whatever the boundaries are.)
        const uint64_t t0 = hid::nowNs();
        raw->addEvent({t0 - 40'000'000, 1, hid::EventType::Button, 0, 0, 1, 0});
        raw->addEvent({t0 - 26'000'000, 1, hid::EventType::Button, 0, 0, 0, 0});
        raw->addEvent({t0 -  9'000'000, 1, hid::EventType::Button, 0, 0, 1, 0});
        engine.tick(3.0f * kSimDt + 0.001f);

        std::vector<simintent::Intent> in;
        for (int t = 2; t >= 0; --t)
            for (const simintent::Intent& i : engine.recordedTick((size_t)t).intents) in.push_back(i);
        CHECK(in.size() == 3, "three ticks ran, one intent each (%zu)", in.size());
        auto bits = [&](size_t t) {
            static char buf[3][16];
            std::snprintf(buf[t], 16, "%s%s%s", in[t].held ? "H" : "-", in[t].pressed ? "P" : "-", in[t].released ? "R" : "-");
            return buf[t];
        };
        if (in.size() == 3) {
            std::printf("        Fire per tick (held/pressed/released): %s %s %s\n", bits(0), bits(1), bits(2));
            CHECK(in[0].held && in[0].pressed && !in[0].released &&
                  !in[1].held && in[1].released && !in[1].pressed &&
                  in[2].held && in[2].pressed && !in[2].released,
                  "each tick saw only its own slice: pressed, then released, then pressed again "
                  "(reading the clock inside the step folded all three into the first tick)");
        }

        engine.stopSimulation();
        engine.shutdown();
    }

    std::printf("\nsim_clock_test: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
