#include "runtime/runtime.h"
#include "runtime/glfw_platform.h"
#include "editor/editor_app.h"
#include "io/project_context.h"
#include "io/cook_service.h"
#include "runtime/logger.h"
#include <assetlib/asset_registry.h>

// Animation subsystem (Phase 1: data structures + math + loader)
#include "animation/skeleton.h"
#include "animation/animation_clip.h"
#include "animation/animation_math.h"
#include "animation/pose.h"
#include "animation/assimp_skeleton_loader.h"

int main(int argc, char** argv) {
    ProjectContext project;
    if (argc > 1 && std::filesystem::is_directory(argv[1])) {
        project = ProjectContext::load(argv[1]);
        LOG_INFO("Project", "Opened from argument: %s", argv[1]);
    } else {
        project = ProjectContext::autoDetect();
        LOG_INFO("Project", "Opened: %s", project.projectRoot.string().c_str());
    }
    project.saveAsLastProject();

    auto dbPath     = project.projectRoot / ".cache" / "registry.db";
    auto assetsRoot = project.projectRoot / "assets";
    auto cacheRoot  = project.projectRoot / ".cache";

    // Main thread registry — read-only after startup scan.
    // CookService opens its own connection for writes (WAL concurrency).
    assetlib::AssetRegistry registry;
    if (registry.open(dbPath)) {
        int n = 0;
        if (std::filesystem::exists(assetsRoot))
            n = registry.scan(assetsRoot, project.projectRoot);
        auto all = registry.all();
        LOG_INFO("AssetLib", "Registry ready — %zu asset(s), %d new/updated",
                 all.size(), n);
    } else {
        LOG_WARN("AssetLib", "Could not open registry at: %s",
                 dbPath.string().c_str());
    }

    // Boot runtime + editor immediately — no blocking cook wait.
    // The editor needs raw GLFW access (ImGui glue, input callbacks), so it
    // creates the platform explicitly and keeps the window pointer; the
    // runtime owns the platform's lifetime.
    auto platform = std::make_unique<GlfwPlatform>();
    GlfwPlatform* glfwPlatform = platform.get();
    EngineRuntime runtime;
    if (!runtime.init({project.name, 1280, 720, 60.0f}, std::move(platform)))
        return 1;

    runtime.ctx().project  = project;
    runtime.ctx().assetLib = &registry;

    // Wire AssetService with project context + asset database
    runtime.assetService().setAssetLib(&registry);
    runtime.assetService().setProjectRoot(project.projectRoot);

    // Wire SceneService cache root (same .cache/ directory)
    runtime.sceneService().setCacheRoot(cacheRoot);

    // Cook service runs in background — editor is already live
    CookService cookService(dbPath, project.projectRoot, assetsRoot, cacheRoot);

    EditorApp editor(runtime, glfwPlatform->glfwWindow());
    editor.init();
    editor.setRegistry(&registry);
    editor.setProjectRoot(project.projectRoot);
    editor.setCookService(&cookService);
    editor.setProject(project);

    cookService.start(); // background thread — doesn't block

    editor.run();

    editor.saveScene();
    project.saveAsLastProject();
    editor.shutdown();
    runtime.shutdown();
    return 0;
}
