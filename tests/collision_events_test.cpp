// ── collision_events_test — contacts reach scripts, with no churn (WO-048) ────
//
// Nothing tested that a collision is delivered to a script at all: only the ABI
// test (its layout) and the determinism gate (its reproducibility) touched
// CollisionEvents. And the way it was published was a hot-path cost: every tick
// the component was SET on bodies with contact events and REMOVED from the
// rest, an archetype move each way (sim_profile: Sim.post 47x for 2x the world).
//
// A box falls onto a floor. Its script launches it upward on its first
// onCollisionEnter, so delivery shows up as motion C++ can see; it then falls
// and lands again, a few times, as a box does.
//   1. the first contact tick's `entered` is exactly [floor]
//   2. the script received it: the box goes up
//   3. no tick lists the floor twice, and enters and exits alternate: the box
//      cannot enter the floor again without having left it
//   4. quiet ticks are EMPTY lists and the component stays: the box's flecs
//      table never changes after its first contact (no archetype churn)
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>

#include <flecs.h>

#include "components/collision_events.h"
#include "components/name.h"
#include "components/rigid_body.h"
#include "components/script_component.h"
#include "core/transform.h"
#include "plugins/jolt_plugin.h"
#include "plugins/lua_script_plugin.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/runtime.h"

namespace fs = std::filesystem;
static int g_failures = 0;
#define CHECK(c, ...) do { if(!(c)){std::printf("  FAIL  " __VA_ARGS__);std::printf("\n");++g_failures;} \
                           else {std::printf("  ok    " __VA_ARGS__);std::printf("\n");} } while(0)

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("collision_events_test\n");
    const fs::path root = fs::temp_directory_path() / "engine_collision_events";
    fs::remove_all(root);
    fs::create_directories(root / "scripts");
    { std::ofstream(root / "scripts" / "bounce.lua") <<
        "local M = {}\n"
        "function M:onStart() self.enters = 0 end\n"
        "function M:onCollisionEnter(other)\n"
        "  self.enters = self.enters + 1\n"
        "  if self.enters == 1 then self.entity:setVelocity(0, 8, 0) end\n"
        "end\n"
        "return M\n"; }

    EngineConfig cfg;
    cfg.openAssetDatabase = false; cfg.autoDetectProject = false; cfg.defaultScene = false;
    cfg.projectRoot = root;
    EngineRuntime engine;
    CHECK(engine.init(cfg, std::make_unique<HeadlessPlatform>()), "a headless runtime");
    engine.plugins().add(std::make_shared<JoltPlugin>());
    engine.plugins().add(std::make_shared<LuaScriptPlugin>());
    engine.attachPlugins();
    flecs::world& w = engine.simWorld();

    RigidBody floor{}; floor.bodyType = PhysicsBodyType::Static; floor.halfExtent = {10, 0.5f, 10};
    const flecs::entity f = w.entity().set<Transform>({{0, -0.5f, 0}, {0, 0, 0, 1}, {1, 1, 1}}).set<RigidBody>(floor);
    RigidBody box{}; box.bodyType = PhysicsBodyType::Dynamic; box.halfExtent = {0.5f, 0.5f, 0.5f}; box.mass = 1;
    const flecs::entity b = w.entity().set<Transform>({{0, 1.0f, 0}, {0, 0, 0, 1}, {1, 1, 1}}).set<RigidBody>(box)
                                      .set<Name>({"box"}).set<ScriptComponent>({"scripts/bounce.lua"});
    engine.startSimulation(EngineRuntime::SimMode::InPlace);

    int firstContactEntered = -1, eventTicks = 0, tableChanges = -1, duplicates = 0, outOfTurn = 0, enters = 0, exits = 0;
    bool onFloor = false, contacted = false;
    const ecs_table_t* table = nullptr;
    float peakAfterContact = -1e9f;
    const int kTicks = 240;
    for (int t = 0; t < kTicks; ++t) {
        engine.tick(1.0f / 60.0f);
        if (const CollisionEvents* ce = b.try_get<CollisionEvents>()) {
            int in = 0, out = 0;
            for (flecs::entity_t o : ce->entered) in  += o == f.id();
            for (flecs::entity_t o : ce->exited)  out += o == f.id();
            if (!contacted) firstContactEntered = (ce->entered.size() == 1 && in == 1) ? 1 : 0;
            duplicates += (in > 1) + (out > 1);
            // Within one tick a contact can end and start again; across ticks
            // the state must alternate: enter only when off, exit only when on.
            if (out) { outOfTurn += !onFloor; onFloor = false; ++exits; }
            if (in)  { outOfTurn += onFloor;  onFloor = true;  ++enters; }
            eventTicks += !ce->entered.empty() || !ce->exited.empty();
            const ecs_table_t* now = ecs_get_table(w.c_ptr(), b.id());
            if (now != table) { ++tableChanges; table = now; }
            contacted = true;
        } else if (contacted) {
            ++tableChanges;   // the component went away after the first contact: churn
            table = nullptr;
        }
        if (contacted) peakAfterContact = std::max(peakAfterContact, b.get<Transform>().position.y);
    }
    engine.stopSimulation();
    engine.shutdown();
    fs::remove_all(root);

    CHECK(firstContactEntered == 1, "the first contact tick's `entered` is exactly [floor]");
    CHECK(peakAfterContact > 1.5f, "the script received onCollisionEnter and launched the box (peak y %.2f)", peakAfterContact);
    CHECK(exits >= 1 && enters >= 2, "it left the floor and landed again (%d enters, %d exits)", enters, exits);
    CHECK(duplicates == 0 && outOfTurn == 0, "no tick lists the floor twice, and enters and exits alternate "
          "(%d duplicated, %d out of turn)", duplicates, outOfTurn);
    CHECK(eventTicks < kTicks / 4, "most ticks are quiet: events on %d of %d ticks", eventTicks, kTicks);
    CHECK(tableChanges == 0, "after its first contact the box never changes archetype (%d moves): quiet ticks are "
          "empty lists, not a removed component", tableChanges);

    std::printf("collision_events_test: %d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
