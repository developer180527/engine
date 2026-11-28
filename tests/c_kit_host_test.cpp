// c_kit_host_test — C modules loaded from a project's manifest (WO-094).
//
// The combat kit (samples/combat_c, built as a shared library against only
// include/eng/) and a counter fixture are named in project.json's "kits" and
// run the way the editor runs a kit at Play:
//   - a host without the C API refuses a C module by name;
//   - with it, each module's on_world_start runs at Play and on_world_stop at
//     Stop, in the simulation's C world, and combat drains damage into Health;
//   - what a module made is gone when it stops, so a module that leaves its
//     system behind still counts once a step after Play again, and after a
//     hot reload.
#include "capi/eng_internal.h"
#include "runtime/platform/headless_platform.h"
#include "runtime/runtime.h"

#include "combat.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
namespace { int g_failures = 0; }
#define CHECK(cond, ...) do {                                       \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);            \
                   std::printf("  (%s:%d: %s)\n", __FILE__, __LINE__, engGetLastErrorMessage()); \
                   ++g_failures; }                                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); } \
} while (0)

namespace {

struct Count { uint32_t steps, starts, stops, pad; };

fs::path makeProject(const fs::path& counter) {
    const fs::path root = fs::temp_directory_path() / "engine_c_kit_host_test";
    fs::remove_all(root);
    fs::create_directories(root / "assets");
    std::ofstream f(root / "project.json");
    f << "{\"name\":\"ckits\",\"assetRoot\":\"assets\",\"kits\":["
      << "{\"name\":\"combat\",\"module\":\"" << C_KIT_COMBAT << "\"},"
      << "{\"name\":\"counter\",\"module\":\"" << counter.generic_string() << "\"}]}";
    return root;
}

bool boot(EngineRuntime& rt, const fs::path& root) {
    EngineConfig cfg;
    cfg.projectRoot = root;
    cfg.defaultScene = false;
    cfg.openAssetDatabase = false;
    if (!rt.init(cfg, std::make_unique<HeadlessPlatform>())) return false;
    rt.attachPlugins();
    return rt.hasProject();
}

// Frames until n more simulation steps have run.
void steps(EngineRuntime& rt, uint64_t n) {
    const uint64_t until = rt.simulationTicks() + n;
    for (int guard = 0; rt.simulationTicks() < until && guard < 10000; ++guard) {
        rt.tickNoRender(1.0f / 60.0f);
    }
}

Count countOf(EngWorld* w) {
    Count c{};
    const EngComponent k = engFindComponent(w, "counter.Count");
    const EngEntity e = engFindEntity(w, "counter");
    if (k && e) engGetComponent(w, e, k, &c, sizeof c);
    return c;
}

const KitHost::KitStatus* statusOf(EngineRuntime& rt, const char* name) {
    for (const auto& s : rt.kits().status())
        if (s.name == name) return &s;
    return nullptr;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("c_kit_host_test: C modules from a project's manifest (WO-094)\n");
    // The counter is copied, so the test can touch it for a hot reload
    // without rebuilding anything.
    const fs::path counter = fs::temp_directory_path() / ("c_module_counter" + fs::path(C_KIT_COUNTER).extension().string());
    fs::copy_file(C_KIT_COUNTER, counter, fs::copy_options::overwrite_existing);
    const fs::path root = makeProject(counter);

    std::printf("\n-- a host without the C API --\n");
    {
        EngineRuntime rt;
        CHECK(boot(rt, root), "the project opens");
        rt.startSimulation();
        const KitHost::KitStatus* s = statusOf(rt, "counter");
        CHECK(!rt.kits().isLoaded("counter") && s && s->state == KitHost::KitStatus::State::LoadFailed,
              "a C module is refused, and the kit says so");
        rt.stopSimulation();
        rt.shutdown();
    }

    std::printf("\n-- Play --\n");
    EngineRuntime rt;
    CHECK(boot(rt, root), "the project opens");
    engInternalHostCModules(&rt);
    CHECK(rt.startSimulation(), "Play");
    CHECK(rt.kits().isLoaded("combat") && rt.kits().isLoaded("counter"), "both C modules load from the manifest");
    // The test holds the simulation's C world too, to look inside it.
    EngWorld* w = engInternalAcquireKitWorld(&rt, rt.simWorld());
    CHECK(countOf(w).starts == 1, "on_world_start ran once, in the world the test sees");
    steps(rt, 5);
    CHECK(countOf(w).steps == 5, "the module's system ran once a step: %u for 5", countOf(w).steps);

    const EngComponent health = engFindComponent(w, COMBAT_HEALTH);
    EngEntity target = 0;
    engCreateEntity(w, "target", &target);
    CHECK(health && engAddComponent(w, target, health, nullptr) == ENG_OK, "the combat kit's Health, on an entity");
    combatDealDamage(w, target, 30.0f);
    steps(rt, 1);
    CombatHealth h{};
    engGetComponent(w, target, health, &h, sizeof h);
    CHECK(h.current == 70.0f, "the combat kit, loaded from a file, drained 30 damage: %g", h.current);

    std::printf("\n-- Stop, and Play again --\n");
    rt.stopSimulation();
    CHECK(countOf(w).stops == 1, "on_world_stop ran at Stop, while the world still worked");
    CHECK(rt.startSimulation(), "Play again");
    w = engInternalAcquireKitWorld(&rt, rt.simWorld());   // the same world: the simulation runs in place
    engInternalReleaseKitWorld(w);
    CHECK(countOf(w).starts == 2, "on_world_start again");
    const uint32_t before = countOf(w).steps;
    steps(rt, 4);
    CHECK(countOf(w).steps - before == 4,
          "the system the module left behind was destroyed at Stop: %u a step, not 2",
          (countOf(w).steps - before) / 4);
    combatDealDamage(w, target, 10.0f);
    steps(rt, 1);
    engGetComponent(w, target, health, &h, sizeof h);
    CHECK(h.current == 60.0f, "and combat applies a hit once: %g", h.current);

    std::printf("\n-- a hot reload --\n");
    fs::last_write_time(counter, fs::last_write_time(counter) + std::chrono::seconds(5));
    steps(rt, 60);   // the watcher looks every 250 ms
    const Count r = countOf(w);
    CHECK(r.starts == 3 && r.stops == 2, "the counter reloaded: stopped and started again (%u, %u)", r.starts, r.stops);
    const uint32_t reloaded = countOf(w).steps;
    steps(rt, 4);
    CHECK(countOf(w).steps - reloaded == 4, "and still counts once a step after the reload");
    CHECK(rt.kits().isLoaded("counter"), "and is still loaded");

    std::printf("\n-- unloaded and loaded again mid-play (the Plug-in Manager) --\n");
    CHECK(rt.kitUnload("counter") && !rt.kits().isLoaded("counter"), "unloaded mid-play");
    const Count off = countOf(w);
    steps(rt, 3);
    CHECK(off.stops == 3 && countOf(w).steps == off.steps, "stopped, and its system gone: no step counted while unloaded");
    CHECK(rt.kitLoad("counter") && rt.kits().isLoaded("counter"), "loaded again mid-play");
    const Count on = countOf(w);
    steps(rt, 3);
    CHECK(on.starts == 4 && countOf(w).steps - on.steps == 3, "started in the running world, and counting once a step");
    combatDealDamage(w, target, 10.0f);
    steps(rt, 1);
    engGetComponent(w, target, health, &h, sizeof h);
    CHECK(h.current == 50.0f, "the other C kit ran on throughout: %g", h.current);

    engInternalReleaseKitWorld(w);
    rt.stopSimulation();

    std::printf("\n-- Play as the editor does: a snapshot, a fresh world --\n");
    const flecs::world_t* inPlace = rt.simWorld().c_ptr();
    CHECK(rt.startSimulation(EngineRuntime::SimMode::Snapshot), "Play in Snapshot mode");
    CHECK(rt.simWorld().c_ptr() != inPlace, "the simulation runs in a world of its own");
    EngWorld* snap = engInternalAcquireKitWorld(&rt, rt.simWorld());
    const EngComponent snapHealth = engFindComponent(snap, COMBAT_HEALTH);
    EngEntity dummy = 0;
    engCreateEntity(snap, "dummy", &dummy);
    CHECK(snapHealth && engAddComponent(snap, dummy, snapHealth, nullptr) == ENG_OK,
          "the kits registered their components in it");
    const uint32_t snapBefore = countOf(snap).steps;
    combatDealDamage(snap, dummy, 25.0f);
    steps(rt, 3);
    engGetComponent(snap, dummy, snapHealth, &h, sizeof h);
    CHECK(h.current == 75.0f && countOf(snap).steps - snapBefore == 3,
          "and their systems run there, once a step (%g, %u)", h.current, countOf(snap).steps - snapBefore);
    engInternalReleaseKitWorld(snap);
    rt.stopSimulation();
    CHECK(rt.simWorld().c_ptr() == inPlace, "Stop: back to the edited world");
    rt.shutdown();
    std::printf(g_failures ? "\nFAILED: %d\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
