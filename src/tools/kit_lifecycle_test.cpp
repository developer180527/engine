// kit_lifecycle_test — headless regression test for mid-play kit unloading.
//
// Repro of the crash it guards against: kits register flecs component hooks
// (ctor template instantiations compiled INTO the kit module). Unloading a kit
// used to dlclose the .so; the world kept the hook pointers, so the next add
// of that component (another kit dealing damage, reflected apply, editor Add
// Component) jumped into unmapped memory. The fix parks retired module images
// in a process-lifetime graveyard (modload::ModuleLibrary::graveyard()).
//
//   usage: kit_lifecycle_test <project-dir>     (expects a "combat" kit)
//
// Flow mirrors the user repro: Play → unload 'combat' mid-play → force a
// ctor-hook call on a combat component → keep ticking (resume) → must survive.
#include "runtime/runtime.h"
#include "runtime/platform/headless_platform.h"
#include "scene/reflected_serde.h"
#include <cstdio>

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: kit_lifecycle_test <project>\n"); return 2; }

    EngineConfig cfg;
    cfg.projectRoot       = argv[1];
    cfg.openAssetDatabase = false;
    cfg.defaultScene      = false;

    EngineRuntime engine;
    if (!engine.init(cfg, std::make_unique<HeadlessPlatform>())) return 1;
    if (!engine.hasProject()) { std::fprintf(stderr, "no project\n"); return 2; }

    engine.startSimulation();                       // kits load + register hooks
    if (!engine.kits().isLoaded("combat")) {
        std::fprintf(stderr, "FAIL: combat kit did not load\n"); return 1;
    }

    float dt = 0.016f;
    for (int i = 0; i < 3; ++i) { engine.frameBegin(dt); engine.tick(dt); engine.frameEnd(); }

    if (!engine.kitUnload("combat")) {              // mid-play unload
        std::fprintf(stderr, "FAIL: kitUnload refused\n"); return 1;
    }

    // The lethal step pre-fix: adding a kit-registered component runs its ctor
    // hook, whose code lived in the now-retired module. This is exactly what a
    // shot after resume does (dealDamage -> set<DamageInbox>).
    flecs::world& w = engine.simWorld();
    flecs::entity health = reflected::lookupPath(w, "combat::Health");
    flecs::entity inbox  = reflected::lookupPath(w, "combat::DamageInbox");
    if (!health.is_valid() || !inbox.is_valid()) {
        std::fprintf(stderr, "FAIL: combat components not registered\n"); return 1;
    }
    flecs::entity e = w.entity();
    if (!reflected::applyBlob(w, e, health, R"({"current": 25, "max": 25})") ||
        !reflected::applyBlob(w, e, inbox,  R"({"pending": 34})")) {
        std::fprintf(stderr, "FAIL: could not add combat components\n"); return 1;
    }

    // "Resume": keep simulating with the components live in the world.
    for (int i = 0; i < 10; ++i) {
        engine.frameBegin(dt);
        engine.tickSimulation(dt);
        engine.tick(dt);
        engine.frameEnd();
    }

    // And a mid-play RE-load — the other Plug-in Manager button.
    if (!engine.kitLoad("combat")) {
        std::fprintf(stderr, "FAIL: mid-play reload refused\n"); return 1;
    }
    for (int i = 0; i < 5; ++i) { engine.frameBegin(dt); engine.tickSimulation(dt);
                                  engine.tick(dt); engine.frameEnd(); }

    engine.stopSimulation();
    engine.shutdown();
    std::printf("kit_lifecycle_test: SURVIVED (unload -> component add -> resume -> reload)\n");
    return 0;
}
