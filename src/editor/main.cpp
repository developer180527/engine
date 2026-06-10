#include "runtime/runtime.h"
#include "runtime/glfw_platform.h"
#include "editor/editor_app.h"
#include "io/project_context.h"
#include "io/cook_service.h"
#include "runtime/logger.h"
#include <assetlib/asset_registry.h>

int main(int argc, char** argv) {
    // The runtime self-wires from EngineConfig: project detection, asset
    // database, services. main() only picks the project root and adds the
    // editor + cook tooling on top.
    EngineConfig cfg;
    if (argc > 1 && std::filesystem::is_directory(argv[1])) {
        cfg.projectRoot = argv[1];
        LOG_INFO("Project", "Opening from argument: %s", argv[1]);
    }

    // The editor needs raw GLFW access (ImGui glue, input callbacks), so it
    // creates the platform explicitly and keeps the window pointer; the
    // runtime owns the platform's lifetime.
    auto platform = std::make_unique<GlfwPlatform>();
    GlfwPlatform* glfwPlatform = platform.get();
    EngineRuntime runtime;
    if (!runtime.init(cfg, std::move(platform)))
        return 1;

    ProjectContext& project = runtime.project();
    project.saveAsLastProject();

    // Cook service runs in background — editor is already live.
    // Dev-time tooling, so it lives with the editor, not the runtime.
    auto cacheRoot = project.projectRoot / ".cache";
    CookService cookService(cacheRoot / "registry.db", project.projectRoot,
                            project.projectRoot / "assets", cacheRoot);

    EditorApp editor(runtime, glfwPlatform->glfwWindow());
    editor.init();
    editor.setRegistry(&runtime.assetLib());
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
