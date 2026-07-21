// engine_cook — cook a project's assets from the command line, no editor.
//
//   engine_cook [project-dir] [--all]
//
// By DEFAULT cooks the scene closure: only the meshes the project's .scene
// files reference (and their textures), not every asset sitting in assets/.
// This is on-demand cooking — a 3-mesh scene no longer melts the machine
// cooking a 637-asset kit. Pass --all to cook the whole registry (the old
// behavior, for a full content bake / CI).
//
// Scans <project>/assets, cooks anything new or stale into <project>/.cache,
// and cooks scene JSON to binary. Exit codes:
//   0  everything cooked (or already up to date)
//   1  one or more assets failed to cook
//   2  no project found
#include "assets/cookers/cook_service.h"
#include "project/project_context.h"
#include "core/logger.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);   // logs stream live to pipes/files

    bool cookAll = false;
    std::string projectArg;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--all") == 0) cookAll = true;
        else if (projectArg.empty())           projectArg = argv[i];
    }

    ProjectContext project;
    if (!projectArg.empty() && std::filesystem::is_directory(projectArg))
        project = ProjectContext::load(projectArg);
    else
        project = ProjectContext::autoDetect();

    if (project.projectRoot.empty() ||
        !std::filesystem::exists(project.projectRoot / "project.json")) {
        std::fprintf(stderr,
            "engine_cook: no project found%s%s\n"
            "usage: engine_cook [project-dir] [--all]\n",
            projectArg.empty() ? " (searched up from cwd)" : " at ",
            projectArg.empty() ? "" : projectArg.c_str());
        return 2;
    }

    LOG_INFO("Cook", "Project: %s  (%s)", project.projectRoot.string().c_str(),
             cookAll ? "whole registry [--all]" : "scene closure");

    const auto cacheRoot = project.projectRoot / ".cache";
    CookService cook(cacheRoot / "registry.db",
                     project.projectRoot,
                     project.assetsRoot,
                     cacheRoot);
    cook.setScope(cookAll ? CookService::Scope::WholeProject
                          : CookService::Scope::SceneClosure);

    const int failed = cook.cookOnce();
    const auto stats = cook.stats();
    LOG_INFO("Cook", "Done — %d cooked, %d failed", stats.cooked, failed);
    return failed > 0 ? 1 : 0;
}
