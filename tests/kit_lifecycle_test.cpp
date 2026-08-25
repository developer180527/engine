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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    // Unbuffered: ctest redirects stdout, which makes it block-buffered,
    // and a test killed on timeout loses everything still in the buffer.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
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

    // ── FAILED hot-reload must not leave a dead entry (audit C.2) ─────────
    // Corrupt the kit image on disk and let the watcher fire: the reload
    // fails the load gauntlet AFTER the old plugin is fully torn down.
    // Pre-fix, the Entry stayed in m_loaded with plugin == nullptr —
    // isLoaded() kept reporting the dead kit healthy (so kitLoad() no-oped
    // forever) and the next stopSimulation() null-derefed. Post-fix the kit
    // is honestly unloaded (status LoadFailed) and a fresh kitLoad recovers.
    {
        std::filesystem::path modPath;
        for (const auto& s : engine.kits().status())
            if (s.name == "combat") modPath = s.resolvedPath;
        if (modPath.empty() || !std::filesystem::exists(modPath)) {
            std::fprintf(stderr, "FAIL: cannot resolve combat module path\n"); return 1;
        }
        // Save the real image; replace with garbage (restored below, before
        // any assertion can exit — the file must never stay corrupt).
        std::vector<char> original;
        {
            std::ifstream in(modPath, std::ios::binary);
            original.assign(std::istreambuf_iterator<char>(in), {});
        }
        { std::ofstream out(modPath, std::ios::binary | std::ios::trunc);
          out << "this is not a loadable module"; }

        // Drive the watcher with EXPLICIT sim dt. frameBegin(dt) overwrites
        // dt with real wall-clock delta — a headless loop spins at ~0.1ms per
        // frame, so wall dt can never accumulate the watcher's 0.25s poll
        // gate. 0.3s per tick: mtime change seen on poll 1, fires on poll 3.
        for (int i = 0; i < 12; ++i) {
            float wall = dt; engine.frameBegin(wall);
            engine.tickSimulation(0.3f);
            engine.tick(dt); engine.frameEnd();
        }

        { std::ofstream out(modPath, std::ios::binary | std::ios::trunc);
          out.write(original.data(), (std::streamsize)original.size()); }

        if (engine.kits().isLoaded("combat")) {
            std::fprintf(stderr, "FAIL: failed reload left kit reported loaded "
                                 "(dead entry in m_loaded)\n"); return 1;
        }
        bool failedSurfaced = false;
        for (const auto& s : engine.kits().status())
            if (s.name == "combat" && s.state == KitHost::KitStatus::State::LoadFailed)
                failedSurfaced = true;
        if (!failedSurfaced) {
            std::fprintf(stderr, "FAIL: reload failure not surfaced as LoadFailed\n");
            return 1;
        }

        // Recovery: the restored image loads back through the normal path.
        if (!engine.kitLoad("combat")) {
            std::fprintf(stderr, "FAIL: recovery load refused after restore\n"); return 1;
        }
        for (int i = 0; i < 5; ++i) { engine.frameBegin(dt); engine.tickSimulation(dt);
                                      engine.tick(dt); engine.frameEnd(); }
    }

    engine.stopSimulation();
    engine.shutdown();
    std::printf("kit_lifecycle_test: SURVIVED (unload -> component add -> resume "
                "-> reload -> failed-reload -> recovery)\n");
    return 0;
}
