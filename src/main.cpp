#include "engine/runtime.h"
#include "editor/editor_app.h"
#include "io/project_context.h"
#include "engine/logger.h"

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
