// profiler_frames — deterministic headless proof of the FRAME profiler path.
// Boots a headless EngineRuntime (no window, no GPU, nothing to throttle),
// ticks a few real frames, and dumps one frame's phase breakdown. Exercises
// the same frameBegin/tick/frameEnd + ENGINE_PROFILE_SCOPE path the windowed
// engine uses.
#include "runtime/runtime.h"
#include "runtime/platform/headless_platform.h"
#include "core/profiler.h"

int main() {
    EngineConfig cfg;
    cfg.autoDetectProject = false;
    cfg.openAssetDatabase = false;
    cfg.defaultScene      = false;   // headless buildDefaultScene no-ops anyway
    cfg.enableProfiler    = true;

    EngineRuntime engine;
    if (!engine.init(cfg, std::make_unique<HeadlessPlatform>())) return 1;

    float dt = 0.016f;
    for (int i = 0; i < 5; ++i) {
        engine.frameBegin(dt);
        engine.tick(dt);            // tickSystems: Animation + ECS.progress
        engine.frameEnd();
    }
    prof::Profiler::get().timer().logLastFrame("Headless frame");
    engine.shutdown();
    return 0;
}
