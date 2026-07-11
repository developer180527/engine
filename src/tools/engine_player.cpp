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
#include <filesystem>
#include <string>

#include "plugins/stock_plugins.h"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);   // logs stream live to pipes/files

    EngineConfig cfg;
    if (argc > 1) cfg.projectRoot = argv[1];
    cfg.defaultScene      = false;   // a game ships its own scene
    cfg.openAssetDatabase = false;   // no registry.db in a shipped build
    cfg.enableProfiler    = false;   // release posture (flip for profiling builds)

    auto platform = std::make_unique<GlfwPlatform>();
    GlfwPlatform* glfwPlat = platform.get();
    EngineRuntime engine;
    if (!engine.init(cfg, std::move(platform))) return 1;
    if (!engine.hasProject()) {
        std::fprintf(stderr, "engine_player: no project.json found%s%s\n",
                     argc > 1 ? " at " : " (run from a project directory "
                     "or pass one)", argc > 1 ? argv[1] : "");
        return 2;
    }

    InputSystem::get().init(glfwPlat->glfwWindow());

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
    engine.run([&](float dt) {
        InputSystem::get().processEvents();
        engine.tick(dt);
    });

    engine.stopSimulation();
    engine.shutdown();
    return 0;
}
