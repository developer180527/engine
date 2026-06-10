// scene_resave — load a project's scene, wait for async imports, save it back.
//
//   scene_resave [project-dir] [input.scene] [output.scene]
//   (input defaults to the project's lastScene)
//
// Two jobs:
//   1. Migration: re-saving upgrades legacy scenes (absolute sourcePath,
//      session-handle skeletonId/clipId) to the AssetRef format
//      (uuid + project-relative path, semantic clipIndex).
//   2. Round-trip test harness for the serializer — output defaults to
//      /tmp/<scene>.resaved so nothing is overwritten unless asked.
#include <engine/engine.h>
#include "io/scene_serializer.h"
#include "runtime/async_loader.h"

#include <cstdio>

int main(int argc, char** argv) {
    EngineConfig cfg;
    if (argc > 1) cfg.projectRoot = argv[1];

    EngineRuntime engine;
    if (!engine.init(cfg)) return 1;
    if (!engine.hasProject()) {
        std::fprintf(stderr, "scene_resave: no project found\n");
        return 2;
    }

    auto& ctx = engine.ctx();
    AssetStorage storage{ctx.assets, ctx.textures, ctx.materials,
                         ctx.skeletons, ctx.clips};

    AsyncLoader loader;
    loader.setRegistry(&engine.assetLib());
    loader.setProjectRoot(engine.project().projectRoot);

    std::filesystem::path scenePath = (argc > 2)
        ? std::filesystem::path(argv[2])
        : engine.project().projectRoot / engine.project().lastScene;
    if (!SceneSerializer::loadAsync(scenePath, ctx.ecs, storage, loader,
                                    ctx.importers, ctx.primitives,
                                    ctx.assetService,
                                    engine.project().projectRoot,
                                    &engine.assetLib()))
        return 1;

    // Pump frames until all async imports drain (cap ~20s just in case).
    float dt = 0.0f;
    int idleFrames = 0;
    for (int i = 0; i < 1200 && idleFrames < 30; ++i) {
        if (!engine.frameBegin(dt)) break;
        bool drained = loader.drainOne(storage);
        idleFrames = (drained || loader.pendingCount() > 0) ? 0 : idleFrames + 1;
        engine.frameEnd();
    }

    std::filesystem::path out = (argc > 3)
        ? std::filesystem::path(argv[3])
        : std::filesystem::path("/tmp") / (scenePath.stem().string() + ".resaved");
    bool ok = SceneSerializer::save(out, ctx.ecs, ctx.assets,
                                    &engine.assetLib(),
                                    engine.project().projectRoot);
    std::printf("scene_resave: %s -> %s (%s)\n",
                scenePath.string().c_str(), out.string().c_str(),
                ok ? "ok" : "FAILED");
    engine.shutdown();
    return ok ? 0 : 1;
}
