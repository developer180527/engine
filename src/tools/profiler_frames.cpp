// profiler_frames — deterministic headless proof of the FRAME profiler path.
// Boots a headless EngineRuntime (no window, no GPU, nothing to throttle),
// ticks real frames, and dumps one frame's phase breakdown. Exercises the same
// frameBegin/tick/frameEnd + ENGINE_PROFILE_SCOPE path the windowed engine uses.
//
//   profiler_frames [project-dir]
//
// With a project: loads its scene, starts simulation (manifest kits load), and
// profiles real game frames — the honest CPU picture minus GPU/vsync.
#include "runtime/runtime.h"
#include "runtime/platform/headless_platform.h"
#include "core/profiler.h"
#include "runtime/mem_channel.h"

int main(int argc, char** argv) {
    EngineConfig cfg;
    cfg.autoDetectProject = false;
    cfg.openAssetDatabase = false;
    cfg.defaultScene      = false;   // headless buildDefaultScene no-ops anyway
    cfg.enableProfiler    = true;
    if (argc > 1) {
        cfg.projectRoot       = argv[1];
        cfg.openAssetDatabase = true;   // scene load resolves through the DB
    }

    EngineRuntime engine;
    if (!engine.init(cfg, std::make_unique<HeadlessPlatform>())) return 1;

    float dt = 0.016f;
    if (engine.hasProject()) {
        // Simulation profile: kits + plugin broadcasts + physics in the frame.
        // The SCENE is deliberately not loaded — asset import still creates GPU
        // buffers (bgfx) even under a headless platform, so content-frame
        // numbers come from a windowed engine_host run (periodic dump) instead.
        engine.startSimulation();
        for (int i = 0; i < 120; ++i) {   // ~2s of simulated frames
            engine.frameBegin(dt);
            engine.tick(dt);              // systems + simulation (+ no-op render)
            engine.frameEnd();
        }
        prof::Profiler::get().timer().logLastFrame("Sim frame (headless)");
        engine.stopSimulation();
    } else {
        for (int i = 0; i < 5; ++i) {
            engine.frameBegin(dt);
            engine.tick(dt);              // tickSystems: Animation + ECS.progress
            engine.frameEnd();
        }
        prof::Profiler::get().timer().logLastFrame("Headless frame");
    }

    // Walk the channel registry and dump the memory channel — proves a
    // consumer can downcast to a known channel type (the overlay pattern).
    for (auto* ch : prof::Profiler::get().channels())
        if (auto* m = dynamic_cast<MemoryChannel*>(ch))
            m->logLastFrame("Headless frame");
    engine.shutdown();
    return 0;
}
