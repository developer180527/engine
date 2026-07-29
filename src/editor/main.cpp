#include "runtime/runtime.h"
#include "runtime/platform/glfw_platform.h"
#include "editor/editor_app.h"
#include "editor/project_hub.h"
#include "editor/editor_theme.h"
#include "editor/imgui/imgui_bgfx.h"
#include "project/project_context.h"
#include "assets/cookers/cook_service.h"
#include "core/logger.h"
#include <assetlib/asset_registry.h>

int main(int argc, char** argv) {
    // Boot flow:
    //   editor <project-dir>  → open that project directly (CI / scripts)
    //   editor                → projectless boot, show the project hub first
    EngineConfig cfg;
    cfg.autoDetectProject = false; // the hub decides, not the cwd
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

    // ImGui comes up before either the hub or the editor draws.
    imguiInit(glfwPlatform->glfwWindow(), 16.0f);   // opaque handle from here on
    applyEditorTheme();

    // ── Project hub — pick or create a project before the editor loads ──
    if (!runtime.hasProject()) {
        ProjectHub hub;
        auto picked = hub.run(runtime);
        if (picked.empty()) {      // window closed from the hub — clean exit
            imguiShutdown();
            runtime.shutdown();
            return 0;
        }
        if (!runtime.openProject(picked)) {
            LOG_ERROR("Project", "Failed to open: %s", picked.string().c_str());
            imguiShutdown();
            runtime.shutdown();
            return 1;
        }
    }

    ProjectContext& project = runtime.project();
    project.saveAsLastProject();
    project::KnownProjects::touch(project.name, project.projectRoot);

    // Cook service runs in background — editor is already live.
    // Dev-time tooling, so it lives with the editor, not the runtime.
    auto cacheRoot = project.projectRoot / ".cache";
    CookService cookService(cacheRoot / "registry.db", project.projectRoot,
                            project.assetsRoot, cacheRoot);

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
