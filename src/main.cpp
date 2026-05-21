#include "engine/runtime.h"
#include "editor/editor_app.h"
#include "io/project_context.h"
#include "engine/logger.h"
#include <assetlib/asset_registry.h>

int main(int argc, char** argv) {
    // Resolve project: argv[1] > last opened > autoDetect
    ProjectContext project;
    if (argc > 1 && std::filesystem::is_directory(argv[1])) {
        project = ProjectContext::load(argv[1]);
        LOG_INFO("Project", "Opened from argument: %s", argv[1]);
    } else {
        project = ProjectContext::autoDetect();
        LOG_INFO("Project", "Opened: %s", project.projectRoot.string().c_str());
    }
    project.saveAsLastProject();

    // ── Asset registry (Milestone A) ─────────────────────────────────────────
    // Scan assets folder, assign stable UUIDs to new files, update hashes.
    // Registry lives at .cache/registry.db — zero engine coupling.
    {
        assetlib::AssetRegistry registry;
        auto dbPath     = project.projectRoot / ".cache" / "registry.db";
        auto assetsRoot = project.projectRoot / "assets";
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
    }

    // Boot runtime with project title
    EngineRuntime runtime;
    if (!runtime.init({project.name, 1280, 720, 60.0f}))
        return 1;

    // Give runtime access to project context
    runtime.ctx().project = project;

    EditorApp editor(runtime);
    editor.init();
    editor.setProject(project);  // loads scene + restores camera
    editor.run();

    // Save on clean exit
    editor.saveScene();
    project.saveAsLastProject();

    editor.shutdown();
    runtime.shutdown();
    return 0;
}
