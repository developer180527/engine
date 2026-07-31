// ── engine_player — the shipped-game runtime ─────────────────────────────────
//
//   engine_player [project-dir]        (default: auto-detect from cwd)
//
// What a PLAYER ships and engine_host doesn't: nothing. What engine_host
// carries and this deliberately does not: dev-module hot-reload + watching,
// --record-input, periodic profiler/heap dumps, and the source-import scene
// path. A shipped game is COOKED — this binary loads binary scenes through
// SceneService and streams cooked meshes through AssetService; it never
// parses FBX/glTF source (see assets/cookers/problems.md, "Runtime
// Philosophy"). If the cooked scene is missing, it says so and points at
// engine_build rather than silently falling back to importers.
//
// Built with ENGINE_WITH_SOURCE_IMPORTERS=OFF this links no Assimp at all.
#include <engine/engine.h>
#include <engine/input.h>

#include <cstdio>
#include <cstdlib>   // strtol — --frames
#include <filesystem>
#include <string>

#include "plugins/stock_plugins.h"
#include "render/render_stats_channel.h"   // --gpu-stats (measure on target)

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);   // logs stream live to pipes/files

    // Diagnostic flags. A shipped player needs these because the numbers that
    // matter are the ones measured ON THE TARGET MACHINE — the whole point of
    // the low-spec box is that our development Mac cannot answer for it. They
    // cost nothing when unused: --gpu-stats reads counters bgfx already keeps,
    // and neither changes the default shipping posture.
    long frameLimit = 0;      // --frames N: stop after N frames (repeatable)
    bool gpuStats   = false;  // --gpu-stats: VRAM / draws / handle churn
    const char* budgetTier = nullptr;   // --budget low|mid|high: PASS/OVER
    std::string projectArg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc)  frameLimit = std::strtol(argv[++i], nullptr, 10);
        else if (a == "--gpu-stats")          gpuStats   = true;
        else if (a == "--budget" && i + 1 < argc) { budgetTier = argv[++i]; gpuStats = true; }
        else if (projectArg.empty() && a[0] != '-') projectArg = a;
    }

    EngineConfig cfg;
    if (!projectArg.empty()) cfg.projectRoot = projectArg;
    cfg.defaultScene      = false;   // a game ships its own scene
    cfg.openAssetDatabase = false;   // no registry.db in a shipped build
    cfg.enableProfiler    = false;   // release posture (flip for profiling builds)

    auto  platform = makeDefaultPlatform();
    auto* plat     = platform.get();
    EngineRuntime engine;
    if (!engine.init(cfg, std::move(platform))) return 1;
    if (!engine.hasProject()) {
        std::fprintf(stderr, "engine_player: no project.json found%s%s\n",
                     argc > 1 ? " at " : " (run from a project directory "
                     "or pass one)", argc > 1 ? argv[1] : "");
        return 2;
    }

#if !defined(ENGINE_WINDOW_BACKEND_SDL3)
    // The window input source is still GLFW-callback based; handing it an
    // SDL_Window* would be a crash. Inert (and safe) under sdl3 until the
    // SDL3 window input source lands.
    InputSystem::get().init(plat->backendWindowHandle());
#endif

    // Engine providers by NAME from project.json's "providers" block —
    // swapping physics/audio is a project setting (stock_plugins.h).
    addStockPlugins(engine);
    engine.attachPlugins();

    // COOKED scene only. The cooked binary carries cooked mesh paths that
    // AssetService streams from .cache — no importer, no Assimp, no source
    // formats anywhere on this path.
    const std::string stem =
        fs::path(engine.project().lastScene).stem().string();
    const std::string cookedRel = "scenes/" + stem + ".cooked";
    const fs::path cookedAbs =
        engine.project().projectRoot / ".cache" / cookedRel;
    if (!fs::exists(cookedAbs)) {
        std::fprintf(stderr,
            "engine_player: cooked scene missing: %s\n"
            "  package the project with engine_build (or open it in the "
            "editor once to cook)\n", cookedAbs.string().c_str());
        return 3;
    }
    if (engine.sceneService().loadScene(cookedRel.c_str()) == 0) {
        std::fprintf(stderr, "engine_player: failed to load %s\n",
                     cookedRel.c_str());
        return 3;
    }

    engine.startSimulation();   // in-place: boot = play

    if (!gpuStats && frameLimit <= 0) {
        engine.run([&](float dt) {          // normal shipping loop, untouched
            InputSystem::get().processEvents();
            engine.tick(dt);
        });
    } else {
        // Measurement loop. Explicit rather than engine.run() so --frames can
        // bound it: a number whose sample length depends on when someone
        // closed the window is not a measurement.
        RenderStatsChannel stats;
        long  frame = 0;
        float dt    = 0.0f;
        while (engine.frameBegin(dt)) {
            InputSystem::get().processEvents();
            engine.tick(dt);
            // Driven directly, not through the profiler hub: the shipping
            // posture leaves the profiler disabled (cfg.enableProfiler=false),
            // and this must work regardless of that.
            if (gpuStats) stats.endFrame();
            engine.frameEnd();
            if (frameLimit > 0 && ++frame >= frameLimit) break;
        }
        if (gpuStats) stats.report("engine_player (SHIPPED path)");
        // --budget turns the measurement into a verdict against real target
        // hardware. Non-zero exit on OVER, so it can gate a build step or run
        // on the low-spec machine itself and fail loudly there.
        if (budgetTier) {
            const auto report = rdiag::evaluate(stats.stats(),
                                                rdiag::parseTier(budgetTier));
            rdiag::printBudget(report);
            if (!report.pass) return 4;
        }
    }

    engine.stopSimulation();
    engine.shutdown();
    return 0;
}
