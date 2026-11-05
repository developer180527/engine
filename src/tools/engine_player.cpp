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
//
// It runs on the engine's C API (include/eng/, WO-070): engCreateEngine,
// engCreateWindow and engRun, or engStep for the measured loop. What it still
// takes from inside the engine is development-side measurement and posture
// (the render stats channel, quietForShipping, finding a project), which the
// C API does not offer yet.
#include <eng/engine.h>
#include "stdout_live.h"

#include <cstdio>
#include <cstdlib>   // strtol — --frames
#include <filesystem>
#include <string>

#include "core/logger.h"                    // quietForShipping
#include "project/project_context.h"       // finding the project from the cwd
#include "render/render_stats_channel.h"   // --gpu-stats (measure on target)

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    stdoutLive();   // logs stream live to pipes/files

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

    // The project: as given, or found from the working directory as before.
    // The engine itself takes an explicit path (eng_engine.h); finding one is
    // this program's job.
    if (projectArg.empty()) {
        const ProjectContext found = ProjectContext::autoDetect();
        if (!found.projectRoot.empty()) projectArg = found.projectRoot.string();
    }
    if (projectArg.empty()) {
        std::fprintf(stderr, "engine_player: no project.json found (run from a project directory "
                             "or pass one)\n");
        return 2;
    }

    // Engine chatter off, warnings and errors kept, the GAME's own categories
    // untouched — a released build should not print engine internals into the
    // player's log, but a game that uses our logger as its logger must keep
    // working. Set before the engine starts so boot logging is covered too.
    elog::quietForShipping();

    // Through the engine's C API (WO-070, DR-0028): the project's cooked main
    // scene, its providers by name from project.json, kits loaded in place (no
    // hot reload in a player), simulation started: what this file did by hand
    // before. A missing project or cooked scene is refused by name.
    EngEngineDesc desc = ENG_INIT(EngEngineDesc);
    desc.api_version  = ENG_API_VERSION;
    desc.app_name     = "engine_player";
    desc.project_path = projectArg.c_str();
    EngEngine* engine = nullptr;
    if (EngResult r = engCreateEngine(&desc, &engine); r != ENG_OK) {
        const std::string why = engGetLastErrorMessage();
        std::fprintf(stderr, "engine_player: %s\n", why.c_str());
        if (why.find("project.json") != std::string::npos) return 2;   // as before: no project
        return 3;                                                      // as before: no cooked scene
    }
    EngWindowDesc wd = ENG_INIT(EngWindowDesc);   // the project's name, 1280x720, windowed
    EngWindow window = 0;
    if (engCreateWindow(engine, &wd, &window) != ENG_OK) {
        std::fprintf(stderr, "engine_player: %s\n", engGetLastErrorMessage());
        engDestroyEngine(engine);
        return 3;
    }

    int exitCode = 0;
    if (!gpuStats && frameLimit <= 0) {
        // The normal shipping loop: pump, step, until the window closes.
        if (engRun(engine, nullptr, nullptr) != ENG_OK) {
            std::fprintf(stderr, "engine_player: %s\n", engGetLastErrorMessage());
            exitCode = 1;
        }
    } else {
        // Measurement loop. Explicit rather than engRun so --frames can
        // bound it: a number whose sample length depends on when someone
        // closed the window is not a measurement.
        RenderStatsChannel stats;
        long frame = 0;
        while (engIsRunning(engine)) {
            engPumpEvents(engine);
            if (engStep(engine, nullptr) != ENG_OK) {
                std::fprintf(stderr, "engine_player: %s\n", engGetLastErrorMessage());
                exitCode = 1;
                break;
            }
            // Driven directly, not through the profiler hub: the shipping
            // posture leaves the profiler disabled, and this must work
            // regardless of that.
            if (gpuStats) stats.endFrame();
            if (frameLimit > 0 && ++frame >= frameLimit) break;
        }
        if (gpuStats) stats.report("engine_player (SHIPPED path)");
        // --budget turns the measurement into a verdict against real target
        // hardware. Non-zero exit on OVER, so it can gate a build step or run
        // on the low-spec machine itself and fail loudly there.
        if (budgetTier && exitCode == 0) {
            const auto report = rdiag::evaluate(stats.stats(),
                                                rdiag::parseTier(budgetTier));
            rdiag::printBudget(report);
            if (!report.pass) exitCode = 4;
        }
    }

    engDestroyEngine(engine);
    return exitCode;
}