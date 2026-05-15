#include "engine/runtime.h"
#include "editor/editor_app.h"

int main() {
    EngineRuntime runtime;
    if (!runtime.init({"Engine [milestone 6]", 1280, 720, 60.0f}))
        return 1;

    EditorApp editor(runtime);
    editor.init();
    editor.run();
    editor.shutdown();

    runtime.shutdown();
    return 0;
}
