// engine_cook — cook a project's assets from the command line, no editor.
//
//   engine_cook [project-dir]
//
// Scans <project>/assets, cooks anything new or stale into
// <project>/.cache, and cooks scene JSON to binary. Exit codes:
//   0  everything cooked (or already up to date)
//   1  one or more assets failed to cook
//   2  no project found
#include "io/cook_service.h"
#include "io/project_context.h"
#include "runtime/logger.h"

#include <cstdio>
#include <filesystem>

int main(int argc, char** argv) {
    ProjectContext project;
    if (argc > 1 && std::filesystem::is_directory(argv[1]))
        project = ProjectContext::load(argv[1]);
    else
        project = ProjectContext::autoDetect();

    if (project.projectRoot.empty() ||
        !std::filesystem::exists(project.projectRoot / "project.json")) {
        std::fprintf(stderr,
            "engine_cook: no project found%s%s\n"
            "usage: engine_cook [project-dir]\n",
            argc > 1 ? " at " : " (searched up from cwd)",
            argc > 1 ? argv[1] : "");
        return 2;
    }

    LOG_INFO("Cook", "Project: %s", project.projectRoot.string().c_str());

    const auto cacheRoot = project.projectRoot / ".cache";
    CookService cook(cacheRoot / "registry.db",
                     project.projectRoot,
                     project.assetsRoot,
                     cacheRoot);

    const int failed = cook.cookOnce();
    const auto stats = cook.stats();
    LOG_INFO("Cook", "Done — %d cooked, %d failed", stats.cooked, failed);
    return failed > 0 ? 1 : 0;
}
